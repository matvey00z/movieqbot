#!/usr/bin/env python3

import argparse
import re
import subprocess
import json
import os
import csv
from enum import Enum

class DBHandler:
    '''
    CSV file: id,text
    '''
    def __init__(self):
        self.dbname = "db.csv"
        self.next_id = 0
        self.tmpdb = {}

    def __enter__(self):
        try:
            with open(self.dbname, "rt") as db:
                reader = csv.reader(db)
                for row in reader:
                    row_id = int(row[0])
                    if row_id >= self.next_id:
                        self.next_id = row_id + 1
        except:
            pass
        self.dbfile = open(self.dbname, "at")
        self.db = csv.writer(self.dbfile)
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.dbfile.close()

    def get_name(self, fid):
        return 'f{:05d}.gif'.format(fid)

    def request_id(self, text):
        ret = self.next_id
        self.next_id += 1
        self.tmpdb[ret] = text
        return ret

    def commit(self, fid):
        self.db.writerow([fid, self.tmpdb[fid]])
        self.dbfile.flush()
        del self.tmpdb[fid]


def easy_run(cmd):
    proc = subprocess.run(cmd, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, check=True,
            universal_newlines=True)
    return proc.stdout

def make_gifs(fname, index, groups, db):
    task_fname = "task.txt"
    report_fname = "report.txt"
    with open(task_fname, "wt") as task_f:
        for group in groups:
            start, end, text = group
            fid = db.request_id(text)
            gifname = db.get_name(fid)
            print('{} {} {} {}'.format(fid, gifname,
                int(start*1000), int(end*1000)), file=task_f)
    filter = ( 'scale=348x216'
              f',subtitles=\'{fname}\':si={index}'
               ':force_style=\'FontSize=32\''
               ',framerate=fps=25'
               ',format=pix_fmts=rgb8')
    cmd = ['cutter', fname, filter, task_fname, report_fname]
    easy_run(cmd)
    id_re = re.compile('^(\d+)')
    with open(report_fname, "rt") as report_f:
        for line in report_f:
            fid = int(id_re.match(line).group(0))
            db.commit(fid)

def get_subtitles_one(fname, index, number, db):
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
    make_gifs(fname, number, groups, db)


def get_subtitles(fname, db):
    cmd = ['ffprobe',
           '-loglevel', 'quiet',
           '-select_streams', 's',
           '-show_streams',
           '-of', 'json=c=1',
           fname]
    streams = json.loads(easy_run(cmd))['streams']
    number = 0
    for stream in streams:
        get_subtitles_one(fname, stream['index'], number, db)
        number += 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('fname', help='Input filename')
    parser.add_argument('dir', help='Working directory')
    args = parser.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    os.chdir(args.dir)
    with DBHandler() as db:
        get_subtitles(args.fname, db)

if __name__ == '__main__':
    main()
