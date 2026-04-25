import os

# Stamp the master pid before gunicorn preloads the app, so main.py's guard
# can tell the master apart from any forked child that inherits the env var
# (workers, dev-mode watcher). This has to run at config-load time: the
# on_starting hook fires after preload, which is too late — main.py is
# already imported and its guard check has already run.
os.environ['GUNICORN_MASTER_PID'] = str(os.getpid())

# Import main.py once in the gunicorn master before forking workers, so the
# UDP listener thread starts in the master only. Each worker inherits state
# without re-running module-level code, so the thread isn't duplicated per
# worker and isn't re-created on worker recycle. This avoids the macOS
# Obj-C fork-safety abort that killed workers when the UDP thread was
# doing DB work at fork() time.
preload_app = True

# Long-lived WebSocket handlers (ws_carpos) sit in a poll/sleep loop and can't
# drain on shutdown, so the default 30s graceful_timeout makes restarts hold
# port 5007 long enough for new gunicorn instances to fail bind and bounce
# under launchd's KeepAlive. Cut workers fast — browsers reconnect WebSockets
# automatically and HTTP requests are short.
graceful_timeout = 2


def post_fork(server, worker):
    # The master opens a MySQL connection at import time (device lookup) which
    # the worker inherits via fork(). Sharing a TCP socket across processes
    # confuses both pymysql and the server — the worker's first ping() can
    # stall for the full TCP timeout before reconnecting. Drop the inherited
    # handles so each worker dials its own connection on first use.
    del server, worker
    import main
    main.db._conn = None
    main.udp_db._conn = None
