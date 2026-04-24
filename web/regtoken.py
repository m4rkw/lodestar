#!/var/www/tracker/.venv/bin/python3
"""Generate a passkey registration URL for a user.

Usage: regtoken.py <username> [hostname]

Prints a single-use registration URL valid for 24 hours.
"""
import os
import sys
import time
import hashlib
import secrets

import pymysql
import pymysql.cursors
import yaml


def main():
    if len(sys.argv) < 2:
        print("usage: regtoken.py <username> [hostname]", file=sys.stderr)
        sys.exit(1)

    username = sys.argv[1]
    hostname = sys.argv[2] if len(sys.argv) >= 3 else 't.rkw.io'

    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config.yaml')) as f:
        config = yaml.safe_load(f)

    db = pymysql.connect(
        host=config['database']['host'],
        port=config['database']['port'],
        user=config['database']['user'],
        password=config['database']['password'],
        database=config['database']['database'],
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )

    token = hashlib.sha256(secrets.token_bytes(32)).hexdigest()
    with db.cursor() as cur:
        cur.execute(
            "INSERT INTO registration (username, token, timestamp) VALUES (%s, %s, %s)",
            (username, token, int(time.time())),
        )

    print(f"https://{hostname}/register?username={username}&token={token}")


if __name__ == '__main__':
    main()
