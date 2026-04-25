#!/var/www/tracker/.venv/bin/python3

from flask import Flask, render_template, request, redirect, Response, session, url_for
from flask_sock import Sock
from functools import wraps
from werkzeug.middleware.proxy_fix import ProxyFix
from base64 import urlsafe_b64encode, urlsafe_b64decode
from webauthn import (
    generate_registration_options,
    options_to_json,
    verify_registration_response,
    generate_authentication_options,
    verify_authentication_response,
)
from webauthn.helpers.structs import (
    AuthenticatorSelectionCriteria,
    UserVerificationRequirement,
    AuthenticatorAttachment,
    RegistrationCredential,
    PublicKeyCredentialRpEntity,
    AuthenticatorAttestationResponse,
    AuthenticatorAssertionResponse,
    AuthenticationCredential,
)
from webauthn.helpers.exceptions import WebAuthnException
import os
import re
import sys
import math
import time
import json
import socket
import secrets
import datetime
import logging
import threading
import base64
import uuid
import pymysql
import pymysql.cursors
import requests as http_requests
import yaml
from dateutil import tz
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.exceptions import InvalidTag

# -- constants ---------------------------------------------------------------

PUSHOVER_URL        = 'https://api.pushover.net/1/messages.json'

with open(os.path.join(os.path.dirname(__file__), 'config.yaml')) as _f:
    _config = yaml.safe_load(_f)

GOOGLE_MAPS_API_KEY = _config['google_maps_api_key']
DEVICE_IMEI         = str(_config['device'])
WORK_CALENDAR_PATH  = _config['work_calendar_path']
PUSHOVER_USER       = _config['pushover']['user']
PUSHOVER_APP        = _config['pushover']['app']
WORK_ALERT_HOUR_FROM = _config.get('work_alert_hour_from', 7)
WORK_ALERT_HOUR_TO   = _config.get('work_alert_hour_to', 18)
SESSION_SECRET      = _config.get('session_secret')
if not SESSION_SECRET:
    raise RuntimeError('session_secret missing from config.yaml')

RATE_LIMIT_REQUEST_COUNT = 5
RATE_LIMIT_RESET_PERIOD  = 3600

UDP_HOST    = _config.get('udp_host', '0.0.0.0')
UDP_PORT    = _config.get('udp_port', 65480)
MAX_DGRAM   = 2048

# Home check: detects tracker stall by comparing the vehicle's last logged
# position against the user's home location when called.
_home_check_cfg       = _config.get('home_check') or {}
HOME_CHECK_DEVICE_ID = _home_check_cfg.get('device_id')
HOME_CHECK_LAT       = _home_check_cfg.get('latitude')
HOME_CHECK_LON       = _home_check_cfg.get('longitude')
HOME_CHECK_RADIUS_M  = _home_check_cfg.get('radius_m', 300)

REQUIRED_KEYS = [
    'latitude', 'longitude', 'speed', 'altitude', 'heading',
    'hdop', 'satellites', 'battery_level', 'gsm_timestamp', 'ignition_state',
]

# Omit always_on and movement_alarm from UDP responses (devices without
# always-on power supply or relay don't need these fields — saves bandwidth)
SLIM_UDP_RESPONSE = False

UPDATE_KEYS = ['int', 'always_on', 'movement_alarm']

# CSV field order: ts,lat,lon,spd,alt,hdg,hdop,sat,bat,ign,waketime,pon[,extras]
# Extras are key=value;key=value groups separated by commas. Accel (ax/ay/az),
# uptime (up), mcu_temp (mt), cell tower (mcc/mnc/lac/cid/cl), config-sync
# (ri/int/ao/ma), debug counters (dbg), and reset cause (rst) all ride there.
CSV_FIELDS = [
    'gsm_timestamp', 'latitude', 'longitude', 'speed', 'altitude',
    'heading', 'hdop', 'satellites',
    'battery_level', 'ignition_state', 'waketime', 'powered_on',
]

# -- logging -----------------------------------------------------------------

logging.basicConfig(
    filename="/var/log/tracker/app.log",
    level=logging.ERROR,
    format="%(asctime)s - %(levelname)s - %(message)s",
)

udp_log = logging.getLogger('udp')
udp_log.setLevel(logging.INFO)
_udp_fmt = logging.Formatter('%(asctime)s %(message)s', datefmt='%Y-%m-%d %H:%M:%S')
_udp_fh = logging.FileHandler('/var/log/tracker/udp.log')
_udp_fh.setFormatter(_udp_fmt)
_udp_sh = logging.StreamHandler()
_udp_sh.setFormatter(_udp_fmt)
udp_log.addHandler(_udp_fh)
udp_log.addHandler(_udp_sh)

# Dedicated log for records that carry debug counters or a reset cause.
# Keeps incident reports out of the bulk udp.log stream.
debug_log = logging.getLogger('debug')
debug_log.setLevel(logging.INFO)
_debug_fh = logging.FileHandler('/var/log/tracker/debug.log')
_debug_fh.setFormatter(_udp_fmt)  # reuse udp formatter — same timestamp style
debug_log.addHandler(_debug_fh)
debug_log.propagate = False  # don't bubble to root logger

# -- database ----------------------------------------------------------------

class DB:
    def __init__(self, config):
        self._config = config
        self._conn = None

    def _connect(self):
        if self._conn is None:
            self._conn = pymysql.connect(
                host=self._config['host'],
                port=self._config['port'],
                database=self._config['database'],
                user=self._config['user'],
                password=self._config['password'],
                cursorclass=pymysql.cursors.DictCursor,
                autocommit=True,
            )
        else:
            self._conn.ping(reconnect=True)
        return self._conn

    def one(self, sql, params=None):
        conn = self._connect()
        with conn.cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchone()

    def all(self, sql, params=None):
        conn = self._connect()
        with conn.cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchall()

    def query(self, sql, params=None):
        conn = self._connect()
        with conn.cursor() as cur:
            cur.execute(sql, params)


DB_CONFIG = _config['database']

db = DB(DB_CONFIG)          # Flask (main thread)
udp_db = DB(DB_CONFIG)      # UDP listener thread

_dev = db.one("SELECT `id` FROM `device` WHERE `imei` = %s", (DEVICE_IMEI,))
if not _dev:
    raise RuntimeError(f"device with IMEI {DEVICE_IMEI} not found in database")
DEVICE_ID = _dev['id']

ENGINE_RUNNING_VOLTAGE = float(_config.get('engine_running_voltage', 13.0))
ENGINE_STOPPED_COUNT = int(_config.get('engine_stopped_count', 10))

# -- shared helpers ----------------------------------------------------------

def lookup_operator(mcc, mnc, database=None):
    """Resolve an mcc/mnc pair to a carrier name via the plmn table."""
    if mcc is None or mnc is None or mcc == '' or mnc == '':
        return None
    if database is None:
        database = db
    try:
        row = database.one(
            "SELECT `operator` FROM `plmn` WHERE `mcc` = %s AND `mnc` = %s LIMIT 1",
            (mcc, mnc),
        )
    except Exception:
        logging.exception('plmn lookup failed for %s/%s', mcc, mnc)
        return None
    return row['operator'] if row else None


def parse_csv_line(line):
    """Parse a CSV telemetry line into dict format.
    Timestamp field contains a comma (dd/mm/yy,HH:MM:SS+NN) so rejoin first two parts."""
    parts = line.split(',')
    if len(parts) > 1:
        parts = [parts[0] + ',' + parts[1]] + parts[2:]

    if len(parts) < len(CSV_FIELDS):
        raise ValueError(f"CSV record has {len(parts)} fields, expected at least {len(CSV_FIELDS)}")

    data = {}
    for i, key in enumerate(CSV_FIELDS):
        data[key] = parts[i]

    for extra in parts[len(CSV_FIELDS):]:
        # dbg=<k:v;k:v...> is a compound field whose body contains
        # semicolons — treat it as one atomic extra and split off any
        # trailing ;rst=<cause> that the firmware emitted.
        if extra.startswith('dbg='):
            body = extra[4:]
            # split off rst= suffix if present
            if ';rst=' in body:
                dbg_part, rst_part = body.rsplit(';rst=', 1)
                data['dbg'] = dbg_part
                data['rst'] = rst_part
            else:
                data['dbg'] = body
            continue

        if extra.startswith('rst='):
            data['rst'] = extra[4:]
            continue

        for kv in extra.split(';'):
            if '=' in kv:
                k, v = kv.split('=', 1)
                if   k == 'ri':  data['request_int']    = v
                elif k == 'int': data['int']             = v
                elif k == 'ao':  data['always_on']       = v
                elif k == 'ma':  data['movement_alarm']  = v
                elif k == 'mcc': data['mcc']             = v
                elif k == 'mnc': data['mnc']             = v
                elif k == 'lac': data['lac']             = v
                elif k == 'cid': data['cid']             = v
                elif k == 'cl':  data['cell_location']   = v
                elif k == 'rat': data['rat']             = v
                elif k == 'ax':  data['accel_x']         = v
                elif k == 'ay':  data['accel_y']         = v
                elif k == 'az':  data['accel_z']         = v
                elif k == 'up':  data['uptime']          = v
                elif k == 'mt':  data['mcu_temp']        = v

    return data


def pushover_send(message, title='Tracker', priority=0, url=None, url_title=None):
    """Send a Pushover notification via the HTTP API."""
    payload = {
        'token': PUSHOVER_APP,
        'user': PUSHOVER_USER,
        'message': message,
        'title': title,
        'priority': priority,
    }
    if priority == 2:
        payload['retry'] = 30
        payload['expire'] = 300
    if url:
        payload['url'] = url
    if url_title:
        payload['url_title'] = url_title
    r = http_requests.post(PUSHOVER_URL, data=payload, timeout=10)
    r.raise_for_status()


def pushover_alert(device_name, alert_msg, priority):
    """Send an alert via Pushover, with URL for location alerts."""
    kwargs = {
        'message': f"{device_name}: {alert_msg}",
        'priority': priority,
    }

    if alert_msg.startswith('google: '):
        coords = alert_msg[8:]
        kwargs['url'] = f"comgooglemaps://?q={coords}"
        kwargs['url_title'] = 'Open in Google Maps'
    elif alert_msg.startswith('tomtom: '):
        coords = alert_msg[8:]
        lat, lon = coords.split(',', 1)
        kwargs['url'] = f"tomtomgo://x-callback-url/navigate?destination={lat},{lon}"
        kwargs['url_title'] = 'Open in TomTom'

    pushover_send(**kwargs)


def is_work_in_office_day():
    """Check if today is a working day where user is in the office (not WFH)."""
    try:
        with open(WORK_CALENDAR_PATH, 'r') as f:
            calendar = yaml.safe_load(f)
        today_key = datetime.datetime.now().strftime('%Y%m%d')
        entry = calendar.get(today_key)
        if entry and entry.get('working') and entry.get('in_office'):
            return True
    except Exception:
        logging.exception('failed to read work calendar')
    return False


def process_record(data, device, ip, database=None):
    """Process a single telemetry record and insert into database.
    Returns a dict with config data if request_int is set, or None."""
    if database is None:
        database = db

    last_log = database.one(
        "select * from `log` where `device_id` = %s order by id desc limit 1",
        (device['id'],),
    )

    entry = {'ip': ip}

    for key in REQUIRED_KEYS:
        if key not in data:
            raise ValueError(f"missing key: {key}")
        if key in ('gsm_timestamp', 'ignition_state'):
            entry[key] = data[key]
        elif key == 'speed':
            entry[key] = f"{float(data[key]) * 0.6213712:.2f}"
        else:
            entry[key] = str(data[key])

    m = re.match(
        r'^(\d+)/(\d+)/(\d+),(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?([+-])(\d+)$',
        entry['gsm_timestamp'],
    )
    if not m:
        raise ValueError('invalid gsm_timestamp format')

    frac = m.group(7)
    microsecond = int(frac.ljust(6, '0')) if frac else 0
    timestamp = datetime.datetime(
        int(m.group(3)) + 2000, int(m.group(2)), int(m.group(1)),
        int(m.group(4)), int(m.group(5)), int(m.group(6)), microsecond,
    )
    now = datetime.datetime.now()

    entry['gsm_timestamp'] = timestamp.strftime('%Y-%m-%d %H:%M:%S.%f')
    entry['gsm_timestamp_offset'] = (
        m.group(9) if m.group(8) == '+' else str(-int(m.group(9)))
    )
    entry['ignition_state'] = 1 if int(entry['ignition_state']) != 0 else 0
    entry['timestamp'] = now.strftime('%Y-%m-%d %H:%M:%S.%f')
    entry['device_id'] = device['id']

    # Copy debug counters / reset cause into the DB row when the firmware
    # included them. Task 13 adds these as columns on the `log` table.
    if 'dbg' in data:
        entry['dbg'] = data['dbg']
    if 'rst' in data:
        entry['rst'] = data['rst']

    # Cell tower info — serving cell at time of send. Firmware only emits
    # these fields when they change, on the first packet after wake, or when
    # cell_location=1 (GPS fallback). When absent, carry forward the last
    # known value from the previous log row so every row has per-point context.
    # cell_location is per-packet semantics (not sticky) — default to 0.
    for k in ('mcc', 'mnc', 'lac', 'cid', 'rat'):
        if k in data:
            entry[k] = data[k]
        elif last_log is not None and last_log.get(k) is not None:
            entry[k] = last_log[k]
    if 'cell_location' in data:
        entry['cell_location'] = data['cell_location']

    # Accelerometer (milli-g), MCU temp (°C), wake/uptime (sec) — per-packet.
    for k in ('accel_x', 'accel_y', 'accel_z', 'mcu_temp', 'uptime', 'waketime'):
        if k in data:
            entry[k] = data[k]

    # Emit a dedicated debug log line so silent-failure incidents are
    # easy to review chronologically without grepping udp.log.
    if 'dbg' in data or 'rst' in data:
        debug_log.info(
            'IMEI=%s up=%s ign=%s bat=%s%s%s',
            device.get('imei', '?'),
            data.get('uptime', '?'),
            entry['ignition_state'],
            entry['battery_level'],
            (' dbg=' + data['dbg']) if 'dbg' in data else '',
            (' rst=' + data['rst']) if 'rst' in data else '',
        )

    powered_on = bool(
        last_log
        and str(last_log['ignition_state']) == '0'
        and str(entry['ignition_state']) == '1'
    )

    if powered_on:
        garage = device.get('garage') or 0
        priority = 0 if garage else 2

        if device.get('alarm'):
            try:
                msg = f"{device['name']} ignition on, battery {entry['battery_level']}V"
                pushover_send(msg, priority=priority)
            except Exception:
                logging.exception('pushover failed for ignition alert')

        if device.get('overnight_alarm'):
            hour_from = device.get('overnight_alarm_hour_from')
            if hour_from is None: hour_from = 23
            hour_to = device.get('overnight_alarm_hour_to')
            if hour_to is None: hour_to = 6
            hour = now.hour
            if hour_from <= hour_to:
                in_window = hour >= hour_from and hour < hour_to
            else:
                in_window = hour >= hour_from or hour < hour_to
            if in_window:
                try:
                    msg = f"{device['name']} overnight ignition on, battery {entry['battery_level']}V"
                    pushover_send(msg, priority=priority)
                except Exception:
                    logging.exception('pushover failed for overnight ignition alert')

        if WORK_ALERT_HOUR_FROM <= now.hour < WORK_ALERT_HOUR_TO and is_work_in_office_day():
            try:
                msg = f"{device['name']} ignition on during work hours, battery {entry['battery_level']}V"
                pushover_send(msg, priority=priority)
            except Exception:
                logging.exception('pushover failed for work hours ignition alert')

    columns = [
        'device_id', 'ip', 'hdop', 'powered_on',
        'latitude', 'longitude', 'altitude', 'speed', 'heading',
        'satellites', 'gsm_timestamp', 'gsm_timestamp_offset',
        'ignition_state', 'battery_level', 'timestamp',
        'dbg', 'rst',
        'mcc', 'mnc', 'lac', 'cid', 'cell_location', 'rat',
        'accel_x', 'accel_y', 'accel_z',
        'waketime', 'uptime', 'mcu_temp',
    ]
    values = [
        entry['device_id'], entry['ip'], entry['hdop'], powered_on,
        entry['latitude'], entry['longitude'], entry['altitude'],
        entry['speed'], entry['heading'],
        entry['satellites'],
        entry['gsm_timestamp'], entry['gsm_timestamp_offset'],
        entry['ignition_state'], entry['battery_level'], entry['timestamp'],
        entry.get('dbg'), entry.get('rst'),
        entry.get('mcc'), entry.get('mnc'),
        entry.get('lac'), entry.get('cid'),
        entry.get('cell_location'),
        entry.get('rat'),
        entry.get('accel_x'), entry.get('accel_y'), entry.get('accel_z'),
        entry.get('waketime'), entry.get('uptime'), entry.get('mcu_temp'),
    ]


    placeholders = ', '.join(['%s'] * len(values))
    column_names = ', '.join(f'`{c}`' for c in columns)
    database.query(f"insert into `log` ({column_names}) values ({placeholders})", values)

    # persist device config when sent by the tracker
    update_fields = []
    update_values = []
    for key in UPDATE_KEYS:
        if key in data:
            update_fields.append(f'`{key}` = %s')
            update_values.append(int(data[key]))
    if update_fields:
        update_values.append(device['id'])
        database.query(
            'update `device` set ' + ', '.join(update_fields) + ' where `id` = %s',
            update_values,
        )

    # return config if requested
    if 'request_int' in data:
        return {
            'int': device.get('int') or 0,
            'ao': device.get('always_on') or 0,
            'ma': device.get('movement_alarm') if device.get('movement_alarm') is not None else 1,
        }

    return None


# -- dev mode auto-reload ----------------------------------------------------

def _get_all_files(directory):
    file_paths = []
    for root, dirs, files in os.walk(directory):
        for f in files:
            file_paths.append(os.path.join(root, f))
    return file_paths

if 'DEV_MODE' in os.environ:
    parent = os.getpid()
    pid = os.fork()

    if pid == 0:
        h = ''
        for path in sorted(_get_all_files(os.path.dirname(__file__))):
            if '.git' in path or not os.path.isfile(path) or path.endswith('.pyc'):
                continue
            h += str(os.stat(path).st_mtime)

        while 1:
            time.sleep(1)
            new_h = ''
            for path in sorted(_get_all_files(os.path.dirname(__file__))):
                if '.git' in path or not os.path.isfile(path) or path.endswith('.pyc'):
                    continue
                new_h += str(os.stat(path).st_mtime)
            if new_h != h:
                os.kill(parent, 9)
                sys.exit()

# -- Flask app ---------------------------------------------------------------

app = Flask(__name__)
app.wsgi_app = ProxyFix(app.wsgi_app, x_for=1, x_host=1, x_port=1, x_proto=1)
app.secret_key = SESSION_SECRET
app.permanent_session_lifetime = datetime.timedelta(days=30)
# Flask's default select_autoescape() skips .tpl; enable for everything so
# user-controlled telemetry fields can't inject script when rendered.
app.jinja_env.autoescape = True
app.config['MAX_CONTENT_LENGTH'] = 1 * 1024 * 1024
sock = Sock(app)


AUDIT_LOG_PATH = '/var/log/tracker/audit.log'


def audit_log(action, detail=''):
    ip = request.headers.get('X-Forwarded-For', request.remote_addr or '')
    username = session.get('username', '')
    ts = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    try:
        with open(AUDIT_LOG_PATH, 'a') as f:
            f.write(f"{ip} - {username} [{ts}] - [{action}] - {detail}\n")
    except Exception:
        logging.exception('audit log write failed')


@app.before_request
def _make_session_permanent():
    session.permanent = True


def login_required(f):
    @wraps(f)
    def wrapper(*args, **kwargs):
        if 'username' not in session:
            return redirect(url_for('login'))
        return f(*args, **kwargs)
    return wrapper


def _rp_id():
    return request.headers.get('X-Forwarded-Host') or request.host.split(':')[0]


def _origin():
    hostname = _rp_id()
    port_hdr = request.headers.get('X-Forwarded-Port')
    proto = request.headers.get('X-Forwarded-Proto', 'https')
    port = int(port_hdr) if port_hdr else (443 if proto == 'https' else 80)
    if (proto == 'https' and port == 443) or (proto == 'http' and port == 80):
        return f'{proto}://{hostname}'
    return f'{proto}://{hostname}:{port}'

def unauth():
    return Response(
        json.dumps({'status': 'error', 'message': 'unauthorised'}, separators=(',', ':')),
        status=401,
        headers={'Content-Type': 'application/json'},
    )

def ok(data={}):
    resp = {'status': 'ok'}
    resp.update(data)
    return Response(
        json.dumps(resp, separators=(',', ':')),
        status=200,
        headers={'Content-Type': 'application/json'},
    )

def error(message):
    return Response(
        json.dumps({'status': 'error', 'message': message}, separators=(',', ':')),
        status=400,
        headers={'Content-Type': 'application/json'},
    )

def _check_bearer():
    header = request.headers.get('Authorization', '')
    if not header.startswith('Bearer '):
        return False
    token = header[7:].strip()
    if not token:
        return False
    row = db.one("SELECT `id` FROM `api_token` WHERE `token` = %s", (token,))
    return row is not None


@app.route('/', methods=['GET'])
def index():
    if 'username' in session:
        return redirect(url_for('car'))
    return redirect(url_for('login'))


@app.route('/login', methods=['GET'])
def login():
    if 'username' in session:
        return redirect(url_for('car'))
    return render_template('login.tpl')


@app.route('/logout', methods=['GET'])
@login_required
def logout():
    audit_log('logout', session.get('username', ''))
    session.pop('username', None)
    return redirect(url_for('login'))


@app.route('/register', methods=['GET', 'POST'])
def register():
    username = request.values.get('username')
    token = request.values.get('token')

    regtoken = db.one(
        "SELECT * FROM `registration` WHERE `username` = %s AND `token` = %s",
        (username, token),
    )

    if not regtoken:
        if request.method == 'POST':
            audit_log('register-error', 'invalid username or token')
            return error('invalid username or token'), 400
        audit_log('registration-link-error', 'invalid username or token')
        return redirect(url_for('login'))

    if 'session_id' not in session:
        session['session_id'] = str(uuid.uuid4())

    if request.method == 'GET':
        audit_log('register-begin', username)
        return render_template('register.tpl', username=username)

    regopts = db.one(
        "SELECT * FROM `regoptions` WHERE `session_id` = %s",
        (session['session_id'],),
    )
    if not regopts:
        audit_log('register-error', 'missing regoptions')
        return error('missing regoptions'), 400

    if time.time() - regopts['timestamp'] > 300:
        audit_log('register-error', 'regoptions expired')
        return error('regoptions expired'), 400

    data = request.get_json()

    try:
        response = AuthenticatorAttestationResponse(
            attestation_object=urlsafe_b64decode(data['response']['attestationObject']),
            client_data_json=urlsafe_b64decode(data['response']['clientDataJSON']),
        )
        credential = RegistrationCredential(
            id=data['id'],
            raw_id=urlsafe_b64decode(data['rawId']),
            response=response,
            type=data['type'],
        )

        stored = json.loads(regopts['regoptions'])
        verification = verify_registration_response(
            credential=credential,
            expected_challenge=stored['challenge'].encode('utf8'),
            expected_origin=_origin(),
            expected_rp_id=_rp_id(),
        )

        db.query(
            "DELETE FROM `registration` WHERE `username` = %s AND `token` = %s",
            (username, token),
        )

        existing = db.one("SELECT * FROM `user` WHERE `username` = %s", (username,))
        if existing:
            db.query("DELETE FROM `user` WHERE `username` = %s", (username,))

        db.query(
            "INSERT INTO `user` (`username`, `user_id`, `credential`) VALUES (%s, %s, %s)",
            (
                username,
                credential.id,
                json.dumps({
                    'credential_id': base64.b64encode(credential.raw_id).decode('ascii'),
                    'public_key': base64.b64encode(verification.credential_public_key).decode('ascii'),
                    'sign_count': verification.sign_count,
                }),
            ),
        )

        audit_log('register-success', username)
        return ok()

    except Exception as e:
        audit_log('register-error', str(e))
        return error(str(e)), 400


@app.route('/regoptions', methods=['POST'])
def regoptions():
    username = request.form.get('username')
    token = request.form.get('token')

    if not username or not token:
        audit_log('regoptions-error', 'missing username or token')
        return error('missing username or token'), 400

    regtoken = db.one(
        "SELECT * FROM `registration` WHERE `username` = %s AND `token` = %s",
        (username, token),
    )
    if not regtoken:
        audit_log('regoptions-error', 'invalid username or token')
        return error('invalid username or token'), 400

    if time.time() - regtoken['timestamp'] >= 86400:
        db.query("DELETE FROM `registration` WHERE `id` = %s", (regtoken['id'],))
        audit_log('regoptions-error', 'token expired')
        return error('token expired'), 400

    if 'session_id' not in session:
        session['session_id'] = str(uuid.uuid4())

    authenticator_selection = AuthenticatorSelectionCriteria(
        authenticator_attachment=AuthenticatorAttachment.PLATFORM,
        user_verification=UserVerificationRequirement.REQUIRED,
        resident_key=UserVerificationRequirement.REQUIRED,
    )

    server_name = _rp_id()
    registration_options = generate_registration_options(
        rp_id=server_name,
        rp_name=server_name,
        user_name=username,
        authenticator_selection=authenticator_selection,
    )

    regoptions_json = options_to_json(registration_options)
    now_ts = int(time.time())
    db.query(
        "INSERT INTO `regoptions` (`session_id`, `regoptions`, `timestamp`) "
        "VALUES (%s, %s, %s) "
        "ON DUPLICATE KEY UPDATE `regoptions` = %s, `timestamp` = %s",
        (session['session_id'], regoptions_json, now_ts, regoptions_json, now_ts),
    )

    audit_log('regoptions-success', username)
    return Response(
        json.dumps({'status': 'ok', 'regoptions': json.loads(regoptions_json)}),
        status=200,
        headers={'Content-Type': 'application/json'},
    )


@app.route('/authoptions', methods=['POST'])
def authoptions():
    if 'session_id' not in session:
        session['session_id'] = str(uuid.uuid4())

    ip = request.headers.get('X-Forwarded-For', request.remote_addr or '')

    ao_ip = db.one("SELECT * FROM `authoptions_ip` WHERE `ip` = %s", (ip,))
    if ao_ip and ao_ip['count'] >= RATE_LIMIT_REQUEST_COUNT:
        if time.time() - ao_ip['last_request_timestamp'] >= RATE_LIMIT_RESET_PERIOD:
            db.query(
                "UPDATE `authoptions_ip` SET `count` = 1, `last_request_timestamp` = %s WHERE `ip` = %s",
                (int(time.time()), ip),
            )
        else:
            audit_log('authoptions-rate-limit', f'IP {ip} exceeded rate limit')
            return Response(
                json.dumps({'status': 'error', 'message': 'too many requests'}),
                status=429,
                headers={'Content-Type': 'application/json'},
            )

    if ao_ip:
        db.query(
            "UPDATE `authoptions_ip` SET `count` = `count` + 1, `last_request_timestamp` = %s WHERE `ip` = %s",
            (int(time.time()), ip),
        )
    else:
        db.query(
            "INSERT INTO `authoptions_ip` (`ip`, `count`, `last_request_timestamp`) VALUES (%s, 1, %s)",
            (ip, int(time.time())),
        )

    data = request.get_json(silent=True) or {}
    username = data.get('username')
    if not username:
        audit_log('authoptions-error', 'missing username')
        return error('invalid request'), 400

    user = db.one("SELECT * FROM `user` WHERE `username` = %s", (username,))
    if not user:
        audit_log('authoptions-error', 'user not found')
        return Response(
            json.dumps({'status': 'error', 'message': 'user not found'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    if user['locked']:
        audit_log('authoptions-error', 'account locked')
        return Response(
            json.dumps({'status': 'error', 'message': 'account locked'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    options = generate_authentication_options(
        rp_id=_rp_id(),
        user_verification=UserVerificationRequirement.PREFERRED,
    )

    challenge_array = [int(b) for b in options.challenge]

    db.query("DELETE FROM `authoptions` WHERE `session_id` = %s", (session['session_id'],))
    db.query(
        "INSERT INTO `authoptions` (user_id, session_id, authoptions, timestamp, useragent, ipaddr) "
        "VALUES (%s, %s, %s, %s, %s, %s)",
        (
            user['user_id'],
            session['session_id'],
            base64.b64encode(options.challenge).decode('ascii'),
            int(time.time()),
            request.headers.get('User-Agent', ''),
            ip,
        ),
    )

    audit_log('authoptions-success', username)
    return Response(
        json.dumps({
            'status': 'ok',
            'authoptions': {
                'challenge': challenge_array,
                'allow_credentials': [],
            },
        }),
        status=200,
        headers={'Content-Type': 'application/json'},
    )


@app.route('/authenticate', methods=['POST'])
def authenticate():
    data = request.get_json(silent=True) or {}

    ao = db.one("SELECT * FROM `authoptions` WHERE `session_id` = %s", (session.get('session_id'),))
    if not ao:
        audit_log('login-error', 'authoptions for session not found')
        return Response(
            json.dumps({'status': 'error', 'message': 'authoptions for session not found'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    userobj = db.one("SELECT * FROM `user` WHERE `user_id` = %s", (data.get('user_id'),))
    if not userobj:
        audit_log('login-error', 'user not found')
        return Response(
            json.dumps({'status': 'error', 'message': 'user not found'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    if userobj['locked']:
        audit_log('login-error', 'account locked')
        return Response(
            json.dumps({'status': 'error', 'message': 'account locked'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    if ao['useragent'] != request.headers.get('User-Agent', ''):
        audit_log('login-error', 'useragent mismatch')
        return Response(
            json.dumps({'status': 'error', 'message': 'useragent mismatch'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    ip = request.headers.get('X-Forwarded-For', request.remote_addr or '')
    if ao['ipaddr'] != ip:
        audit_log('login-error', 'IP mismatch')
        return Response(
            json.dumps({'status': 'error', 'message': 'IP mismatch'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    if time.time() - ao['timestamp'] >= 300:
        db.query("DELETE FROM `authoptions` WHERE `session_id` = %s", (session['session_id'],))
        audit_log('login-error', 'session expired')
        return Response(
            json.dumps({'status': 'error', 'message': 'session expired'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )

    challenge = base64.b64decode(ao['authoptions'])
    _cred = json.loads(userobj['credential'])
    user = {
        'credential_id': base64.b64decode(_cred['credential_id']),
        'public_key': base64.b64decode(_cred['public_key']),
        'sign_count': _cred['sign_count'],
    }
    auth_data = data['authentication_data']

    try:
        response = AuthenticatorAssertionResponse(
            authenticator_data=urlsafe_b64decode(auth_data['response']['authenticatorData']),
            client_data_json=urlsafe_b64decode(auth_data['response']['clientDataJSON']),
            signature=urlsafe_b64decode(auth_data['response']['signature']),
        )
        credential = AuthenticationCredential(
            id=data.get('user_id'),
            raw_id=urlsafe_b64decode(auth_data['rawId']),
            response=response,
            type='public-key',
        )

        verify_authentication_response(
            credential=credential,
            expected_challenge=challenge,
            expected_origin=_origin(),
            expected_rp_id=_rp_id(),
            credential_public_key=user['public_key'],
            credential_current_sign_count=user['sign_count'],
        )

        db.query("DELETE FROM `authoptions` WHERE `session_id` = %s", (session['session_id'],))
        db.query("UPDATE `user` SET failed_login_count = 0 WHERE `id` = %s", (userobj['id'],))
        db.query("UPDATE `authoptions_ip` SET `count` = 0 WHERE `ip` = %s", (ip,))

        session['username'] = userobj['username']
        audit_log('login-success', userobj['username'])

        return ok({'message': 'authentication successful'})

    except WebAuthnException as e:
        new_count = userobj['failed_login_count'] + 1
        if new_count >= 5:
            db.query(
                "UPDATE `user` SET failed_login_count = %s, locked = 1 WHERE `id` = %s",
                (new_count, userobj['id']),
            )
        else:
            db.query(
                "UPDATE `user` SET failed_login_count = %s WHERE `id` = %s",
                (new_count, userobj['id']),
            )
        audit_log('login-error', f'WebAuthn verification failed: {e}, failed_login_count={new_count}')
        return Response(
            json.dumps({'status': 'error', 'message': 'authentication failed'}),
            status=401,
            headers={'Content-Type': 'application/json'},
        )


@app.route('/api/1.0/track', defaults={'path': ''}, methods=['GET'])
@app.route('/<path:path>', methods=['GET'])
def track(path):
    if not _check_bearer():
        return unauth()

    now = (datetime.datetime.now() + datetime.timedelta(hours=1)).strftime('%Y-%m-%d %H:%M:%S')
    row = db.one("select * from `log` where `device_id` = %s and gsm_timestamp <= %s order by gsm_timestamp desc limit 1", [2, now])
    coords = f"{row['latitude']},{row['longitude']}"

    if request.args.get('google'):
        url = f"https://maps.google.co.uk/maps/place/{coords}/"
    else:
        url = f"maps:ll={coords}&q=car"

    if request.args.get('return'):
        return url

    return redirect(url)


@app.route('/api/1.0/home', methods=['POST'])
def home_check():
    if not _check_bearer():
        return unauth()

    if HOME_CHECK_DEVICE_ID is None or HOME_CHECK_LAT is None or HOME_CHECK_LON is None:
        return error('home_check not configured')

    last_log = db.one(
        "select latitude, longitude from `log` where device_id = %s order by id desc limit 1",
        (HOME_CHECK_DEVICE_ID,),
    )
    if not last_log:
        return error('no log entries for device')

    lat = float(last_log['latitude'])
    lon = float(last_log['longitude'])

    # Haversine distance from home
    lat1 = math.radians(HOME_CHECK_LAT)
    lat2 = math.radians(lat)
    dlat = lat2 - lat1
    dlon = math.radians(lon - HOME_CHECK_LON)
    a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    distance_m = 2 * 6371000 * math.asin(math.sqrt(a))

    at_home = distance_m <= HOME_CHECK_RADIUS_M

    device = db.one("select garage from `device` where id = %s", (HOME_CHECK_DEVICE_ID,))
    garage = (device or {}).get('garage') or 0

    if not at_home and not garage:
        try:
            pushover_send(
                f"Tracker may be stalled — vehicle is {int(distance_m)}m from home",
                title='Tracker home check',
                priority=0,
            )
        except Exception:
            logging.exception('pushover failed for home check')

    return ok({'at_home': at_home, 'distance_m': round(distance_m, 1), 'garage': bool(garage)})


def _lookup_device(imei=None, device_id=None):
    imei = imei or request.headers.get('X-Imei')
    if imei:
        return db.one("SELECT * FROM `device` WHERE `imei` = %s", (imei,))
    if device_id is not None:
        return db.one("SELECT * FROM `device` WHERE `id` = %s", (device_id,))
    return None


@app.route('/api/1.0/config', methods=['GET'])
def get_config():
    if not _check_bearer():
        return unauth()

    device = _lookup_device(
        imei=request.args.get('imei'),
        device_id=request.args.get('device_id'),
    )
    if not device:
        return error('device not found')

    return ok({
        'int': device.get('int') or 0,
        'ao': device.get('always_on') or 0,
        'ma': device.get('movement_alarm') if device.get('movement_alarm') is not None else 1,
        'al': device.get('alarm') or 0,
        'ga': device.get('garage') or 0,
        'oa': device.get('overnight_alarm') or 0,
        'oaf': device.get('overnight_alarm_hour_from') if device.get('overnight_alarm_hour_from') is not None else 23,
        'oat': device.get('overnight_alarm_hour_to') if device.get('overnight_alarm_hour_to') is not None else 6,
    })


@app.route('/api/1.0/config', methods=['POST'])
def update_config():
    if not _check_bearer():
        return unauth()

    data = request.get_json(silent=True)
    if not data:
        return error('invalid JSON body')

    device = _lookup_device(imei=data.get('imei'), device_id=data.get('device_id'))
    if not device:
        return error('device not found')

    field_map = {'int': 'int', 'ao': 'always_on', 'ma': 'movement_alarm', 'al': 'alarm',
                 'oa': 'overnight_alarm', 'oaf': 'overnight_alarm_hour_from', 'oat': 'overnight_alarm_hour_to'}
    update_fields = []
    update_values = []
    for key, col in field_map.items():
        if key in data:
            update_fields.append(f"`{col}` = %s")
            update_values.append(int(data[key]))

    if update_fields:
        update_values.append(device['id'])
        db.query(
            "update `device` set " + ", ".join(update_fields) + " where `id` = %s",
            update_values,
        )

    return ok()


def _extract_command_keys(cmd_str):
    """Extract command key names from a command string.
    e.g. 'int=3600' → {'int'}, 'locate' → {'locate'}, 'int=60,alarm=1' → {'int','alarm'}"""
    keys = set()
    for part in cmd_str.split(','):
        part = part.strip()
        if '=' in part:
            keys.add(part.split('=', 1)[0])
        elif part:
            keys.add(part)
    return keys

# Commands that supersede each other
_COMMAND_CONFLICTS = {
    'locate':    {'locate', 'locatenow'},
    'locatenow': {'locate', 'locatenow'},
    'tomtom':    {'tomtom', 'tomtomnow'},
    'tomtomnow': {'tomtom', 'tomtomnow'},
    'poweroff':  {'poweroff', 'alwayson'},
    'alwayson':  {'alwayson', 'poweroff'},
}

def _get_conflict_keys(keys):
    """Expand keys to include all conflicting command names."""
    conflict = set()
    for k in keys:
        conflict.update(_COMMAND_CONFLICTS.get(k, {k}))
    return conflict

def _dedup_commands(device_id, new_command, database=None):
    """Delete any queued commands for device that conflict with new_command."""
    if database is None:
        database = db
    new_keys = _extract_command_keys(new_command)
    conflict_keys = _get_conflict_keys(new_keys)

    existing = database.all(
        "SELECT `id`, `command` FROM `command` WHERE `device_id` = %s",
        (device_id,),
    )
    for cmd in existing:
        existing_keys = _extract_command_keys(cmd['command'])
        if existing_keys & conflict_keys:
            database.query("DELETE FROM `command` WHERE `id` = %s", (cmd['id'],))


@app.route('/api/1.0/command', methods=['POST'])
def queue_command():
    if not _check_bearer():
        return unauth()

    data = request.get_json(silent=True)
    if not data or 'command' not in data:
        return error('command required')

    # look up device by imei or device_id
    device = None
    if 'imei' in data:
        device = db.one("SELECT `id` FROM `device` WHERE `imei` = %s", (data['imei'],))
    elif 'device_id' in data:
        device = db.one("SELECT `id` FROM `device` WHERE `id` = %s", (data['device_id'],))

    if not device:
        return error('device not found')

    # alarm-related commands update the device table directly (server-side alerting)
    _ALARM_COMMANDS = {
        'alarm': 'alarm',
        'garage': 'garage',
        'overnightalarm': 'overnight_alarm',
        'overnight_alarm_hour_from': 'overnight_alarm_hour_from',
        'overnight_alarm_hour_to': 'overnight_alarm_hour_to',
    }
    device_parts = []   # applied directly to device table
    tracker_parts = []  # queued for tracker

    for part in data['command'].split(','):
        part = part.strip()
        if '=' in part:
            key = part.split('=', 1)[0]
            if key in _ALARM_COMMANDS:
                device_parts.append(part)
                continue
        tracker_parts.append(part)

    if device_parts:
        update_fields = []
        update_values = []
        for part in device_parts:
            key, val = part.split('=', 1)
            col = _ALARM_COMMANDS[key]
            update_fields.append(f'`{col}` = %s')
            update_values.append(int(val))
        update_values.append(device['id'])
        db.query(
            'UPDATE `device` SET ' + ', '.join(update_fields) + ' WHERE `id` = %s',
            update_values,
        )

    if tracker_parts:
        cmd_str = ','.join(tracker_parts)
        _dedup_commands(device['id'], cmd_str)
        db.query(
            "INSERT INTO `command` (`device_id`, `timestamp`, `command`) VALUES (%s, NOW(6), %s)",
            (device['id'], cmd_str),
        )

    return ok()


@app.route('/api/1.0/car', methods=['GET'])
@login_required
def car():
    device = db.one("SELECT `registration` FROM `device` WHERE `id` = %s", (DEVICE_ID,))

    now = (datetime.datetime.now() + datetime.timedelta(hours=1)).strftime('%Y-%m-%d %H:%M:%S')
    log = db.one("select * from `log` where `device_id` = %s and gsm_timestamp <= %s order by gsm_timestamp desc limit 1", [2, now])

    ts = log['timestamp']
    if ts:
        log['display_date'] = ts.strftime('%d.%m.%Y')
        log['display_timestamp'] = ts.strftime('%H:%M:%S')
    else:
        log['display_date'] = ''
        log['display_timestamp'] = ''

    # Determine engine_running with hysteresis: only consider engine stopped
    # if the last ENGINE_STOPPED_COUNT readings were all below the threshold
    engine_running = False
    if log['ignition_state'] == 1:
        recent = db.all(
            "SELECT battery_level FROM `log` WHERE device_id = %s ORDER BY id DESC LIMIT %s",
            [DEVICE_ID, ENGINE_STOPPED_COUNT],
        )
        engine_running = any(
            float(r['battery_level']) >= ENGINE_RUNNING_VOLTAGE for r in recent
        )

    registration = device['registration'] if device else 'Unknown'
    operator = lookup_operator(log.get('mcc'), log.get('mnc')) or ''
    return render_template('track.tpl', registration=registration, engine_running=engine_running,
                           engine_running_voltage=ENGINE_RUNNING_VOLTAGE,
                           engine_stopped_count=ENGINE_STOPPED_COUNT, log=log,
                           operator=operator,
                           google_maps_api_key=GOOGLE_MAPS_API_KEY)


@app.route('/api/1.0/journeys', methods=['GET'])
@login_required
def journeys():
    page = int(request.args.get('page', 0))
    per_page = 50
    offset = page * per_page

    rows = db.all(
        "SELECT id, start_time, end_time, from_latitude, from_longitude, "
        "to_latitude, to_longitude, miles, from_place, to_place "
        "FROM journey WHERE device_id = %s ORDER BY start_time DESC LIMIT %s OFFSET %s",
        (DEVICE_ID, per_page, offset),
    )
    result = []
    for r in rows:
        result.append({
            'id': r['id'],
            'start_time': r['start_time'].strftime('%Y-%m-%d %H:%M:%S') if r['start_time'] else None,
            'end_time': r['end_time'].strftime('%Y-%m-%d %H:%M:%S') if r['end_time'] else None,
            'from_latitude': float(r['from_latitude']) if r['from_latitude'] else 0,
            'from_longitude': float(r['from_longitude']) if r['from_longitude'] else 0,
            'to_latitude': float(r['to_latitude']) if r['to_latitude'] else 0,
            'to_longitude': float(r['to_longitude']) if r['to_longitude'] else 0,
            'miles': float(r['miles']) if r['miles'] else 0,
            'from_place': r['from_place'] or '',
            'to_place': r['to_place'] or '',
        })
    return json.dumps(result), 200, {'Content-Type': 'application/json'}


@app.route('/api/1.0/journey/<int:journey_id>/points', methods=['GET'])
@login_required
def journey_points(journey_id):
    journey = db.one("SELECT * FROM journey WHERE id = %s", (journey_id,))
    if not journey:
        return error('journey not found')

    # journey.end_time is DATETIME (second precision) but log.timestamp is
    # DATETIME(6); compare against end_time+1s so the ignition-off row at
    # the same second isn't excluded by its microseconds.
    rows = db.all(
        "SELECT latitude, longitude, speed, altitude, heading, timestamp, ignition_state, battery_level, mcc, mnc, rat "
        "FROM `log` WHERE device_id = %s AND timestamp >= %s AND timestamp < %s + INTERVAL 1 SECOND "
        "ORDER BY timestamp ASC",
        (DEVICE_ID, journey['start_time'], journey['end_time']),
    )
    operator_cache = {}
    result = []
    for r in rows:
        key = (r.get('mcc'), r.get('mnc'))
        if key not in operator_cache:
            operator_cache[key] = lookup_operator(r.get('mcc'), r.get('mnc')) or ''
        result.append({
            'latitude': float(r['latitude']) if r['latitude'] else 0,
            'longitude': float(r['longitude']) if r['longitude'] else 0,
            'speed': float(r['speed']) if r['speed'] else 0,
            'altitude': float(r['altitude']) if r['altitude'] else 0,
            'heading': float(r['heading']) if r['heading'] else 0,
            'timestamp': r['timestamp'].strftime('%d.%m.%Y %H:%M:%S') if r['timestamp'] else '',
            'ignition_state': r['ignition_state'],
            'battery_level': float(r['battery_level']),
            'operator': operator_cache[key],
            'rat': r.get('rat') or '',
        })
    return json.dumps(result), 200, {'Content-Type': 'application/json'}


@app.route('/api/1.0/carpos', methods=['GET'])
@login_required
def carpos():
    now = (datetime.datetime.now() + datetime.timedelta(hours=1)).strftime('%Y-%m-%d %H:%M:%S')
    last_log = db.one("select * from `log` where `device_id` = %s and gsm_timestamp <= %s order by gsm_timestamp desc limit 1", [2, now])

    operator = lookup_operator(last_log.get('mcc'), last_log.get('mnc')) or ''

    result = {
        "latitude": float(last_log['latitude']) if last_log['latitude'] else 0,
        "longitude": float(last_log['longitude']) if last_log['longitude'] else 0,
        "speed": float(last_log['speed']) if last_log['speed'] else 0,
        "altitude": float(last_log['altitude']) if last_log['altitude'] else 0,
        "heading": float(last_log['heading']) if last_log['heading'] else 0,
        "timestamp": last_log['timestamp'].strftime("%d.%m.%Y %H:%M:%S"),
        "battery_level": float(last_log['battery_level']) if last_log['battery_level'] else 0,
        "ignition_state": last_log['ignition_state'],
        "operator": operator,
        "rat": last_log.get('rat') or '',
    }
    return json.dumps(result), 200, {'Content-Type': 'application/json'}


# -- WebSocket endpoint ------------------------------------------------------

@sock.route('/ws/carpos')
def ws_carpos(ws):
    """Stream car position updates to the client in real time."""
    if 'username' not in session:
        ws.close(1008, 'unauthorised')
        return

    ws_db = DB(DB_CONFIG)
    last_id = 0
    last_ping = time.time()
    while ws.connected:
        row = ws_db.one(
            "select * from `log` where `device_id` = %s and `id` > %s order by `id` desc limit 1",
            (DEVICE_ID, last_id),
        )

        data = None
        if row:
            last_id = row['id']
            operator = lookup_operator(row.get('mcc'), row.get('mnc'), database=ws_db) or ''
            data = {
                'latitude': float(row['latitude']) if row['latitude'] else 0,
                'longitude': float(row['longitude']) if row['longitude'] else 0,
                'speed': float(row['speed']) if row['speed'] else 0,
                'altitude': float(row['altitude']) if row['altitude'] else 0,
                'heading': float(row['heading']) if row['heading'] else 0,
                'timestamp': row['timestamp'].strftime('%d.%m.%Y %H:%M:%S') if row['timestamp'] else '',
                'battery_level': float(row['battery_level']) if row['battery_level'] else 0,
                'ignition_state': row['ignition_state'],
                'operator': operator,
                'rat': row.get('rat') or '',
            }

        try:
            if data is not None:
                ws.send(json.dumps(data, separators=(',', ':')))
                last_ping = time.time()
            elif time.time() - last_ping >= 10:
                ws.send('{"ping":true}')
                last_ping = time.time()
        except Exception:
            return

        time.sleep(1)


# -- UDP listener ------------------------------------------------------------

# Wire format (binary, one envelope per datagram):
#   request:  [1] imei_len  [imei_len] IMEI ASCII  [12] nonce  [N] ct  [16] tag
#   response: [12] nonce  [N] ct  [16] tag
# AAD on both directions = the device IMEI bytes.
# Plaintext = the same '\n'-separated CSV records / response strings used
# pre-crypto, so all parsing logic downstream of decrypt is unchanged.

NONCE_BYTES = 12
TAG_BYTES = 16

# In-memory nonce-replay window per device; the most recent nonce is also
# persisted (device.last_nonce) so a server restart can't allow the single
# most-recent captured packet to be replayed.
_seen_nonces = {}
_NONCE_WINDOW = 1024
_seen_nonces_lock = threading.Lock()


def _nonce_seen(device_id, nonce):
    """Return True if this nonce has been seen recently for this device.
    Updates both the in-memory window and the persisted last_nonce."""
    with _seen_nonces_lock:
        win = _seen_nonces.get(device_id)
        if win is None:
            # First datagram from this device since process start — seed the
            # window from the persisted last_nonce so we still reject a same-
            # second replay across a restart.
            persisted = udp_db.one(
                'SELECT last_nonce FROM `device` WHERE `id` = %s',
                (device_id,),
            )
            seed = persisted and persisted.get('last_nonce')
            win = [bytes(seed)] if seed else []
            _seen_nonces[device_id] = win

        if nonce in win:
            return True
        win.append(nonce)
        if len(win) > _NONCE_WINDOW:
            del win[0]

    try:
        udp_db.query(
            'UPDATE `device` SET `last_nonce` = %s WHERE `id` = %s',
            (nonce, device_id),
        )
    except Exception:
        udp_log.exception('failed to persist last_nonce for device %s', device_id)
    return False


def _device_aead(device):
    psk_hex = device.get('psk') or ''
    if len(psk_hex) != 64:
        return None
    try:
        return ChaCha20Poly1305(bytes.fromhex(psk_hex))
    except ValueError:
        return None


def _decrypt_request(raw):
    """Parse and decrypt a request envelope. Returns (device, plaintext,
    req_nonce) or (None, None, None) on any failure (malformed, unknown IMEI,
    bad PSK, bad tag, replay)."""
    if len(raw) < 1 + NONCE_BYTES + TAG_BYTES:
        return None, None, None
    imei_len = raw[0]
    if imei_len < 14 or imei_len > 16:
        return None, None, None
    if len(raw) < 1 + imei_len + NONCE_BYTES + TAG_BYTES:
        return None, None, None

    imei_bytes = bytes(raw[1:1 + imei_len])
    try:
        imei = imei_bytes.decode('ascii')
    except UnicodeDecodeError:
        return None, None, None
    if not imei.isdigit():
        return None, None, None

    nonce = bytes(raw[1 + imei_len:1 + imei_len + NONCE_BYTES])
    ct_and_tag = bytes(raw[1 + imei_len + NONCE_BYTES:])

    device = udp_db.one('select * from `device` where `imei` = %s', (imei,))
    if not device:
        return None, None, None

    aead = _device_aead(device)
    if aead is None:
        return None, None, None

    try:
        pt = aead.decrypt(nonce, ct_and_tag, imei_bytes)
    except InvalidTag:
        return None, None, None

    if _nonce_seen(device['id'], nonce):
        return None, None, None

    return device, pt, nonce


def _encrypt_response(device, plaintext, req_nonce):
    """Seal a response bound to the request's nonce. The device verifies that
    its cached request-nonce matches the AAD; otherwise AEAD auth fails, so a
    replayed response from a prior exchange is rejected."""
    aead = _device_aead(device)
    if aead is None:
        return None
    nonce = secrets.token_bytes(NONCE_BYTES)
    aad = device['imei'].encode('ascii') + req_nonce
    ct = aead.encrypt(nonce, plaintext.encode('ascii'), aad)
    return nonce + ct


_decrypt_fail_counters = {}
_decrypt_fail_lock = threading.Lock()
_DECRYPT_FAIL_WINDOW_SEC = 60
_DECRYPT_FAIL_LOG_EVERY = 20


def _should_log_decrypt_fail(ip):
    """Log the first failure per IP per window, then 1-in-N after that.
    Prevents a spoofed-UDP flood from filling udp.log."""
    now = time.monotonic()
    with _decrypt_fail_lock:
        entry = _decrypt_fail_counters.get(ip)
        if entry is None or now - entry[0] >= _DECRYPT_FAIL_WINDOW_SEC:
            _decrypt_fail_counters[ip] = [now, 1]
            return True
        entry[1] += 1
        return entry[1] % _DECRYPT_FAIL_LOG_EVERY == 0


def handle_datagram(raw, addr):
    """Process a UDP datagram and return response bytes (or None on failure)."""
    ip = addr[0]
    device, pt, req_nonce = _decrypt_request(raw)
    if device is None:
        if _should_log_decrypt_fail(ip):
            udp_log.warning('decrypt failed from %s (%d bytes)', ip, len(raw))
        return None

    imei = device['imei']
    text = pt.decode('ascii', errors='replace')
    lines = [l for l in text.split('\n') if l.strip()]

    processed = 0
    for line in lines:
        try:
            if line.startswith('A,'):
                # alert record: A,priority,message
                parts = line[2:].split(',', 1)
                if len(parts) == 2 and parts[0].lstrip('-').isdigit():
                    priority = int(parts[0])
                    alert_msg = parts[1]
                else:
                    priority = 0
                    alert_msg = line[2:]
                garage = device.get('garage') or 0
                if garage and priority == 2:
                    priority = 0
                udp_log.info('alert from %s (pri=%d): %s', imei, priority, alert_msg)
                try:
                    pushover_alert(device['name'], alert_msg, priority)
                except Exception:
                    udp_log.exception('pushover failed for alert')
                processed += 1
            else:
                process_record(parse_csv_line(line), device, ip, database=udp_db)
                processed += 1
        except ValueError as e:
            udp_log.error('record error IMEI=%s: %s', imei, e)
        except Exception:
            udp_log.exception('unexpected error IMEI=%s', imei)

    udp_log.info('%d records from %s (%s)', processed, imei, ip)

    # re-fetch device to get config updated by process_record
    device = udp_db.one('SELECT * FROM `device` WHERE `id` = %s', (device['id'],))

    # build compact response
    # full:  1,int,ao,ma[,cmd]   (SLIM_UDP_RESPONSE = False)
    # slim:  1,int[,cmd]         (SLIM_UDP_RESPONSE = True)
    r_int = device.get('int') or 0
    if SLIM_UDP_RESPONSE:
        resp = f'1,{r_int}'
    else:
        r_ao = device.get('always_on') or 0
        r_ma = device.get('movement_alarm') if device.get('movement_alarm') is not None else 1
        resp = f'1,{r_int},{r_ao},{r_ma}'

    # fetch and delete pending commands
    commands = udp_db.all(
        "SELECT `id`, `command` FROM `command` WHERE `device_id` = %s ORDER BY `id`",
        (device['id'],),
    )
    if commands:
        cmd_str = ','.join(c['command'] for c in commands)
        resp += f',{cmd_str}'
        cmd_ids = [c['id'] for c in commands]
        placeholders = ','.join(['%s'] * len(cmd_ids))
        udp_db.query(f"DELETE FROM `command` WHERE `id` IN ({placeholders})", cmd_ids)
        udp_log.info('delivered %d commands to %s: %s', len(commands), imei, cmd_str)

    return _encrypt_response(device, resp, req_nonce)


def udp_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    max_retries = 30
    delay = 1
    for attempt in range(1, max_retries + 1):
        try:
            sock.bind((UDP_HOST, UDP_PORT))
            break
        except OSError as e:
            udp_log.error('UDP bind failed (attempt %d/%d): %s', attempt, max_retries, e)
            if attempt == max_retries:
                udp_log.critical('UDP bind failed after %d attempts, exiting', max_retries)
                os._exit(1)
            time.sleep(min(delay, 30))
            delay *= 2

    udp_log.info('UDP listening on %s:%d', UDP_HOST, UDP_PORT)

    while True:
        try:
            data, addr = sock.recvfrom(MAX_DGRAM)
            response = handle_datagram(data, addr)
            if response:
                sock.sendto(response, addr)
        except OSError:
            break
        except Exception:
            udp_log.exception('UDP loop error')


# Start the UDP listener in the gunicorn master only. With preload_app=True
# the master runs this module at import time; workers inherit state without
# re-executing it, and the dev-mode watcher fork will have a different pid.
# Keeping the thread out of worker processes avoids the macOS Obj-C
# fork-safety abort when workers recycle.
if os.environ.get('GUNICORN_MASTER_PID') == str(os.getpid()):
    _udp_thread = threading.Thread(target=udp_listener, daemon=True)
    _udp_thread.start()
