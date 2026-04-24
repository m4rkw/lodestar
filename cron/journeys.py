#!/usr/bin/env python

import sys
import math
import requests
import json
import re
import time
import os
import datetime
import traceback
import smtplib
from db import DB

JOURNEY_CUTOFF_SECONDS  = 300
SPEED_THRESHOLD         = 3.0   # mph – below this, treat as stationary
EARTH_RADIUS_KM         = 6371.0

os.environ['TZ'] = 'Europe/London'

db = DB({
    'host': '127.0.0.1',
    'port': 3306,
    'database': 'tracker',
    'user': '',
    'password': ''
})

class Journeys:

    def main(self):
        last_journey = db.one("select * from journey where device_id = %s order by `end_time` desc limit 1", [2,])

        if last_journey:
            last_journey_end = last_journey['end_time'].strftime('%Y-%m-%d %H:%M:%S')
        else:
            last_journey_end = '2019-01-01 00:00:00'

        self.process_journeys(last_journey_end)


    def _finish_journey(self, journey):
        """Finalise and save a journey if it has enough points."""
        if len(journey['points']) > 2:
            journey['to_latitude'] = journey['points'][-1]['latitude']
            journey['to_longitude'] = journey['points'][-1]['longitude']
            journey['end_time'] = journey['points'][-1]['timestamp']
            self.process_journey(journey)

    def _new_journey(self, row):
        return {
            'start_time': row['timestamp'],
            'from_latitude': row['latitude'],
            'from_longitude': row['longitude'],
            'miles': 0,
            'points': [row]
        }

    def process_journeys(self, last_journey_end):
        journey = {}
        last_timestamp = None

        data = db.query("select * from `log` where device_id = %s and `timestamp` > %s order by `timestamp` asc", [2, last_journey_end])

        for row in data:
            if journey == {}:
                # Only start a journey when ignition is on
                if row.get('ignition_state'):
                    journey = self._new_journey(row)
                last_timestamp = row['timestamp']
                continue

            # Ignition turned off — end the current journey
            if not row.get('ignition_state'):
                journey['points'].append(row)
                self._finish_journey(journey)
                journey = {}
                last_timestamp = row['timestamp']
                continue

            delta = (row['timestamp'] - last_timestamp).total_seconds()

            if delta >= JOURNEY_CUTOFF_SECONDS:
                # There's a time gap. Check whether we were still moving —
                # if both sides of the gap show meaningful speed, the tracker
                # probably just missed a few reports and it's one journey.
                prev_speed = float(journey['points'][-1].get('speed') or 0)
                curr_speed = float(row.get('speed') or 0)

                if prev_speed < SPEED_THRESHOLD and curr_speed < SPEED_THRESHOLD:
                    # Stationary on both sides of the gap — split
                    self._finish_journey(journey)
                    journey = self._new_journey(row)
                else:
                    # Still moving — keep the journey going
                    journey['points'].append(row)
            else:
                journey['points'].append(row)

            last_timestamp = row['timestamp']

        # Handle any in-progress journey
        if journey and len(journey['points']) > 2:
            t1 = datetime.datetime.now()
            t2 = journey['points'][-1]['timestamp']

            if (t1 - t2).total_seconds() >= JOURNEY_CUTOFF_SECONDS:
                self._finish_journey(journey)


    def haversine(self, lat1, lon1, lat2, lon2):
        """Return the distance in km between two GPS points using the Haversine formula."""
        lat1, lon1, lat2, lon2 = map(math.radians, [lat1, lon1, lat2, lon2])
        dlat = lat2 - lat1
        dlon = lon2 - lon1
        a = math.sin(dlat / 2) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
        return 2 * EARTH_RADIUS_KM * math.asin(math.sqrt(a))


    def total_distance(self, coordinates):
        """Return total distance in miles for a list of (lat, lon) tuples."""
        return sum(
            self.haversine(*coordinates[i], *coordinates[i + 1])
            for i in range(len(coordinates) - 1)
        ) * 0.621371


    def process_journey(self, journey):
        coords = []

        for point in journey['points']:
            coords.append((float(point['latitude']), float(point['longitude'])))

        miles = self.total_distance(coords)

        if '%.2f' % (miles) != '0.00':
            db.query("INSERT INTO journey (device_id, start_time, end_time, from_latitude, from_longitude, to_latitude, to_longitude, miles) VALUES (%s, %s, %s, %s, %s, %s, %s, %s)", [
                2,
                journey['start_time'].strftime('%Y-%m-%d %H:%M:%S'),
                journey['end_time'].strftime('%Y-%m-%d %H:%M:%S'),
                journey['from_latitude'],
                journey['from_longitude'],
                journey['to_latitude'],
                journey['to_longitude'],
                miles
            ])


j = Journeys()
j.main()
