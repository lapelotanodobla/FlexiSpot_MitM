#!/usr/bin/env python3
"""Decode a desk-sniffer capture into an annotated timeline of transitions."""
import re, sys

SEG = {0x3F:'0',0x06:'1',0x5B:'2',0x4F:'3',0x66:'4',0x6D:'5',0x7D:'6',0x07:'7',0x7F:'8',0x6F:'9',0x00:' ',0x40:'-',0x79:'E',0x50:'r',0x76:'H',0x38:'L',0x5C:'o',0x78:'t',0x54:'n',0x73:'P',0x77:'A',0x7C:'b',0x39:'C',0x5E:'d'}

def seg(b):
    c = SEG.get(b & 0x7F, '?')
    return c + ('.' if b & 0x80 else '')

line_re = re.compile(r'^\[(\d\d:\d\d:\d\d\.\d\d\d)\]\[.\]\[(DESK|KEYPAD|binary_sensor)[^\]]*\]: (.*)$')

events = []          # (ts, text)
last_key = None      # last keypad payload
last_disp = None     # last display string
key_count = 0
key_start = None
counts = {}

def flush_key(ts):
    global last_key, key_count, key_start
    if last_key is not None and last_key != '00:00':
        events.append((key_start, f"KEYPAD command payload {last_key} held ({key_count} frames, until {ts})"))
    last_key = None; key_count = 0; key_start = None

for line in open(sys.argv[1]):
    m = line_re.match(line.strip())
    if not m: continue
    ts, tag, body = m.groups()
    if tag == 'binary_sensor':
        if '>>' in body:
            events.append((ts, f"PIN20 {body.split('>>')[1].strip()}"))
            if 'OFF' in body: flush_key(ts)
        continue
    parts = body.split(':')
    if len(parts) < 4 or parts[0] != '9B':
        if body not in ('00','FE'):
            events.append((ts, f"{tag} non-frame bytes: {body}"))
        else:
            events.append((ts, f"{tag} wake transient: {body}"))
        continue
    typ = parts[2]
    counts[(tag,typ)] = counts.get((tag,typ),0)+1
    if tag == 'KEYPAD' and typ == '02':
        payload = ':'.join(parts[3:5])
        if payload != last_key:
            flush_key(ts)
            last_key = payload; key_count = 1; key_start = ts
            if payload != '00:00':
                events.append((ts, f"KEYPAD key DOWN payload={payload}"))
            else:
                events.append((ts, f"KEYPAD keys released"))
        else:
            key_count += 1
    elif tag == 'DESK' and typ == '12':
        disp = ''.join(seg(int(p,16)) for p in parts[4:7])
        if disp != last_disp:
            events.append((ts, f"DISPLAY -> \"{disp}\""))
            last_disp = disp
    elif tag == 'DESK' and typ in ('13','15','11'):
        pass  # heartbeats counted, not timeline-worthy
    else:
        events.append((ts, f"{tag} UNSEEN type={typ}: {body}"))

for ts, text in events:
    print(f"{ts}  {text}")
print("\n--- frame counts by (bus, type) ---")
for k in sorted(counts): print(f"{k[0]:7s} type {k[1]}: {counts[k]}")
