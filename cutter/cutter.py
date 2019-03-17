#!/usr/bin/env python3

import argparse
import re
import subprocess
import json
import os
import sqlite3
from enum import Enum
import hashlib

class DBHandler:
    '''
    Table gifs
        id    int64
        fname string
        text  string
        start int64 # msec
        length it64 # msec
        tg_id int64 # reserved
        tg_file_id string
    Table movies
        id       int64
        hash     string
        sub_id   int8
        start_id int64
        end_id   int64
    Table failed_gifs
        gif_id   int64
    '''
    def __init__(self):
        self.dbname = "db.sqlite3"

    def __enter__(self):
        self.conn = sqlite3.connect(self.dbname)
        cursor = self.conn.cursor()
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS gifs
            (id         int  NOT NULL PRIMARY KEY,
             name       text NOT NULL UNIQUE,
             text       text NOT NULL,
             start      int  NOT NULL,
             end        int  NOT NULL,
             tg_file_id text UNIQUE)
            ''')
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS movies
            (id       INTEGER  NOT NULL PRIMARY KEY AUTOINCREMENT,
             hash     text     NOT NULL,
             sub_id   int      NOT NULL,
             start_id int      NOT NULL UNIQUE,
             end_id   int      NOT NULL UNIQUE,
             UNIQUE (hash, sub_id))
            ''')
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS failed_gifs
            (gif_id int NOT NULL PRIMARY KEY)''')
        self.conn.commit()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.conn.commit()
        self.conn.close()

    def request_movie(self, hash, sub_id, groups_cnt):
        '''
        Search DB for existing gifs for this movie.
        Return (start_id, offset).
        offset is the number for the first group to use
        start_id is the id for the first group to use
        '''
        cursor = self.conn.cursor()
        cursor.execute('''
            SELECT id, start_id, end_id FROM movies
            WHERE hash = ? AND sub_id = ?''',
            (hash, sub_id))
        rows = cursor.fetchall()
        assert len(rows) <= 1
        if len(rows) != 0:
            movie_id, start_id, end_id = rows[0]
            orig_start_id = start_id
            cursor.execute('''
                SELECT id from gifs
                WHERE id >= ? AND id <= ?
                ORDER BY id DESC LIMIT 1''',
                (start_id, end_id))
            rows = cursor.fetchall()
            assert len(rows) <= 1
            if len(rows) != 0:
                start_id = max(start_id, rows[0][0] + 1)
            cursor.execute('''
                SELECT gif_id FROM failed_gifs
                WHERE gif_id >= ? AND gif_id <= ?
                ORDER BY gif_id DESC LIMIT 1''',
                (start_id, end_id))
            rows = cursor.fetchall()
            assert len(rows) <= 1
            if len(rows) != 0:
                print(rows)
                failed_id = rows[0][0]
                print('File {}: found failed gif {} (id {})'
                        .format(hash, failed_id - orig_start_id, failed_id))
                start_id = max(start_id, failed_id + 1)
            print('File {}: resuming from gif {} (id {})'
                    .format(hash, start_id - orig_start_id, start_id))
            return start_id, start_id - orig_start_id
        cursor.execute('''SELECT end_id FROM movies
                          ORDER BY end_id DESC LIMIT 1''')
        rows = cursor.fetchall()
        assert len(rows) <= 1
        if len(rows) == 0:
            start_id = 0
        else:
            start_id = rows[0][0] + 1
        cursor.execute('''
            INSERT INTO movies
            (hash, sub_id, start_id, end_id)
            VALUES(?, ?, ?, ?)''',
            (hash, sub_id, start_id, start_id + groups_cnt - 1))
        self.conn.commit()
        return (start_id, 0)

    def report_ok(self, fid, fname, text, start, end):
        cursor = self.conn.cursor()
        cursor.execute('''
            INSERT INTO gifs
            (id, name, text, start, end)
            VALUES(?, ?, ?, ?, ?)''',
            (fid, fname, text, int(start*1000), int(end*1000)))

    def report_failed(self, fid):
        cursor = self.conn.cursor()
        cursor.execute('''
            INSERT INTO failed_gifs
            (gif_id)
            VALUES(?)''',
            (fid,))


def get_name(fid):
    return 'f{:08d}.mp4'.format(fid)


def calculate_hash(fname):
    BUFSIZE = 16*1024*1024
    h = hashlib.sha256()
    with open(fname, 'rb') as f:
        for data in iter(lambda: f.read(BUFSIZE), b''):
            h.update(data)
    return h.hexdigest()


def easy_run(cmd, check=True):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, check=check,
            universal_newlines=True)
    return proc.stdout

def make_gifs(fname, index, groups, start_id, db):
    '''
    Return True on success, False on faliure
    '''
    if len(groups) == 0:
        return True
    task_fname = "task.txt"
    report_fname = "report.txt"
    try:
        os.remove(report_fname)
    except OSError:
        pass
    fid = start_id
    with open(task_fname, "wt") as task_f:
        for group in groups:
            start, end, text = group
            gifname = get_name(fid)
            print('{} {} {} {}'.format(fid, gifname,
                int(start*1000), int(end*1000)), file=task_f)
            fid += 1

    filter = ( 'scale=348x216'
              f',subtitles=\'{fname}\':si={index}'
               ':force_style=\'FontSize=32\''
               ',framerate=fps=25')
    cmd = ['cutter', fname, filter, task_fname, report_fname]
    easy_run(cmd, check=False)
    id_re = re.compile('^(\d+)')
    next_fid = start_id
    max_fid = start_id + len(groups) - 1
    groups_iter = (g for g in groups)
    with open(report_fname, "rt") as report_f:
        for line in report_f:
            assert next_fid <= max_fid
            fid = int(id_re.match(line).group(0))
            assert fid >= next_fid
            for failed_id in range(next_fid, fid):
                db.report_failed(failed_id)
                next(groups_iter)
            start, end, text = next(groups_iter)
            db.report_ok(fid, get_name(fid), text, start, end)
            next_fid = fid + 1
    if next_fid < max_fid:
        db.report_failed(next_fid)
        return False
    return True

def get_subtitles_one(fname, hash, index, number, db):
    '''
    Return True on success, False on faliure
    '''
    cmd = ['ffmpeg',
           '-i', fname,
           '-map', '0:' + str(index),
           '-f', 'srt',
           'pipe:1']
    srt = easy_run(cmd)
    subtitles = []
    class Mode(Enum):
        NUMBER = 1
        TIME = 2
        NEWLINE = 3
    mode = Mode.NUMBER
    number_re = re.compile('^\d+$')
    time_re = re.compile(('^'
        '(\d{1,2}):(\d{1,2}):(\d{1,2}),(\d+)'
        '\s+-->\s+'
        '(\d{1,2}):(\d{1,2}):(\d{1,2}),(\d+)$'))
    start_time = None
    end_time   = None
    lines = []
    for line in srt.splitlines():
        if mode == Mode.NUMBER:
            match = number_re.match(line)
            if match is not None:
                mode = Mode.TIME
            continue
        elif mode == Mode.TIME:
            match = time_re.match(line)
            if match is None:
                continue
            sh, sm, ss, sf, eh, em, es, ef = match.groups()
            start_time = (float(sh) * 3600 + float(sm) * 60 + float(ss) +
                          float(sf) / 10**len(sf))
            end_time   = (float(eh) * 3600 + float(em) * 60 + float(es) +
                          float(ef) / 10**len(ef))
            mode = Mode.NEWLINE
            continue
        elif mode == Mode.NEWLINE:
            if len(line) == 0:
                subtitles.append((start_time, end_time, '\n'.join(lines)))
                start_time = None
                end_time   = None
                lines      = []
                mode = Mode.NUMBER
                continue
            lines.append(line)
    minlen = 2
    maxlen = 15
    # A group is a tuple (start, end, text)
    groups = []
    open_groups = []
    for subtitle in subtitles:
        start, end, text = subtitle
        new_open_groups = []
        for group in open_groups:
            gstart, gend, gtext = group
            if end - gstart > maxlen:
                # close group
                if gend - gstart < minlen:
                    gend = gstart + minlen
                groups.append((gstart, gend, gtext))
            else:
                # keep group and open a new one
                new_open_groups.append(group)
                gstart, gend, gtext = group
                new_open_groups.append((gstart, end, gtext + '\n' + text))
        open_groups = new_open_groups
        open_groups.append((start, end, text))
    start_id, offset = db.request_movie(hash, number, len(groups))
    return make_gifs(fname, number, groups[offset:], start_id, db)


def get_subtitles(fname, db):
    hash = calculate_hash(fname)
    cmd = ['ffprobe',
           '-loglevel', 'quiet',
           '-select_streams', 's',
           '-show_streams',
           '-of', 'json=c=1',
           fname]
    streams = json.loads(easy_run(cmd))['streams']
    number = 0
    MAX_ATTEMPTS = 5
    for stream in streams:
        for attempt in range(MAX_ATTEMPTS):
            if get_subtitles_one(fname, hash, stream['index'], number, db):
                break
        number += 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('fname', help='Input filename')
    parser.add_argument('dir', help='Working directory')
    args = parser.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    os.chdir(args.dir)
    with DBHandler() as db:
        pass
        get_subtitles(args.fname, db)

if __name__ == '__main__':
    main()
