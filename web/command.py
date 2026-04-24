#!/usr/bin/env python3
"""Queue a command for a tracker device.

Usage: command.py <imei> <command> [command2] ...
Example: command.py 867698040012345 int=3600
         command.py 867698040012345 movealarm=0
"""
import os
import sys
import yaml
import pymysql
import pymysql.cursors
import requests

with open(os.path.join(os.path.dirname(__file__), 'config.yaml')) as _f:
    _config = yaml.safe_load(_f)

SERVER = _config.get('command_server', 'http://localhost:65480')


def _get_token():
    """Fetch any valid bearer token from the api_token table."""
    db = pymysql.connect(
        host=_config['database']['host'],
        port=_config['database']['port'],
        user=_config['database']['user'],
        password=_config['database']['password'],
        database=_config['database']['database'],
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )
    with db.cursor() as cur:
        cur.execute("SELECT `token` FROM `api_token` ORDER BY `id` LIMIT 1")
        row = cur.fetchone()
    if not row:
        print("Error: no token in api_token table — run gentoken.py first", file=sys.stderr)
        sys.exit(1)
    return row['token']


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <imei> <command> [command2] ...")
        print(f"Example: {sys.argv[0]} 867698040012345 int=3600")
        print()
        print("Commands: int=<seconds>, movealarm=0|1, alwayson=0|1,")
        print("          alarm=0|1, overnightalarm=0|1, overnight_alarm_hour_from=0-23,")
        print("          overnight_alarm_hour_to=0-23,")
        print("          locate, locatenow, config, reboot, poweroff")
        sys.exit(1)

    imei = sys.argv[1]
    command = ','.join(sys.argv[2:])

    token = _get_token()

    url = f'{SERVER}/api/1.0/command'
    r = requests.post(
        url,
        json={'imei': imei, 'command': command},
        headers={'Authorization': f'Bearer {token}'},
        timeout=10,
    )

    if not r.text:
        print(f"Error: empty response (HTTP {r.status_code}) from {url}")
        sys.exit(1)

    try:
        data = r.json()
    except requests.exceptions.JSONDecodeError:
        print(f"Error: invalid response (HTTP {r.status_code}): {r.text[:200]}")
        sys.exit(1)

    if data.get('status') == 'ok':
        print(f"Queued: {command}")
    else:
        print(f"Error: {data.get('message', 'unknown')}")
        sys.exit(1)


if __name__ == '__main__':
    main()
