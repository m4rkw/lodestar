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
