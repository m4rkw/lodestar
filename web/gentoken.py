#!/var/www/tracker/.venv/bin/python3
"""Generate a static bearer token and insert it into the api_token table.

Usage: gentoken.py <name>

Any token present in the api_token table is valid. Pass it to the server
in the Authorization header as: Authorization: Bearer <token>

Prints the generated token on stdout.
"""
import os
import sys
import time
import secrets

import pymysql
import pymysql.cursors
import yaml


def main():
    if len(sys.argv) != 2:
        print("usage: gentoken.py <name>", file=sys.stderr)
        sys.exit(1)

    name = sys.argv[1]

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

    token = secrets.token_urlsafe(32)
    with db.cursor() as cur:
        cur.execute(
            "INSERT INTO `api_token` (`name`, `token`, `created_at`) VALUES (%s, %s, %s)",
            (name, token, int(time.time())),
        )

    print(token)


if __name__ == '__main__':
    main()
