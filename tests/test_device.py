"""Integration checks against a running ESP32. Restores its original controls."""
import argparse
import json
import time
import urllib.error
import urllib.parse
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('url', nargs='?', default='http://192.168.0.122')
args = parser.parse_args()
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def request(path, body=None):
    data = None if body is None else (body if isinstance(body, str) else urllib.parse.urlencode(body)).encode()
    req = urllib.request.Request(args.url.rstrip('/') + path, data=data,
        headers={'Content-Type': 'application/x-www-form-urlencoded'} if data is not None else {})
    try:
        with opener.open(req, timeout=8) as response:
            text = response.read().decode()
            return response.status, json.loads(text) if response.headers.get_content_type() == 'application/json' else text
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()

def status():
    code, data = request('/api/status')
    assert code == 200, (code, data)
    return data

def configure(**values):
    code, data = request('/api/control', values)
    assert code == 200, (values, code, data)
    return data

original = status()['control']
keys = ['stop_c', 'start_c', 'full_c', 'offset_c', 'min_percent', 'max_percent']
try:
    configure(auto_enabled=0, strategy="threshold", stop_c=35, start_c=40, full_c=60, offset_c=0, min_percent=30, max_percent=100)
    s = status()
    assert s['temperature_valid'] and isinstance(s['temperature_c'], (int, float))
    assert s['temperature_age_ms'] <= 1500 and s['connected']
    for minutes in (5, 10, 20, 30):
        code, data = request('/api/history?minutes=' + str(minutes))
        assert code == 200 and data['minutes'] == minutes and data['sample_period_s'] == 5
        assert 1 <= len(data['samples']) <= 361
        ages = [row[0] for row in data['samples']]
        assert ages == sorted(ages, reverse=True) and max(ages) <= minutes * 60
    for suffix in ('?minutes=1', '?minutes=60', '?minutes=5&minutes=10', '?foo=5'):
        assert request('/api/history' + suffix)[0] == 400
    print('Temperature and all four history windows OK', flush=True)
    before = status()['control']
    for body in ['', 'unknown=1', 'auto_enabled=2', 'auto_enabled=1&auto_enabled=0',
                 'curve_deadband_c=-1', 'curve_deadband_c=11', 'curve_deadband_c=nan', 'start_c=nan', 'start_c=inf', 'start_c=bad', 'offset_c=21',
                 'min_percent=20.5', 'min_percent=256', 'max_percent=101',
                 'stop_c=40&start_c=40', 'full_c=30', 'min_percent=70&max_percent=60']:
        assert request('/api/control', body)[0] == 400, body
        assert status()['control'] == before, body
    print('Invalid, duplicate, non-finite, and inconsistent settings rejected atomically', flush=True)
    t = status()['temperature_c']
    s = configure(auto_enabled=1, stop_c=round(t+6, 1), start_c=round(t+8, 1), full_c=round(t+18, 1), min_percent=30, max_percent=100, offset_c=0)
    assert s['target_percent'] == 0 and s['applied_percent'] == 0 and not s['auto_running']
    assert request('/api/fan', {'percent': 50})[0] == 409
    print('Automatic cooling cutoff and manual-write conflict OK', flush=True)
    t = status()['temperature_c']
    s = configure(stop_c=round(t-8, 1), start_c=round(t-3, 1), full_c=round(t+7, 1))
    assert s['auto_running'] and 30 <= s['target_percent'] < 100
    assert s['startup'] and s['applied_percent'] == 100
    time.sleep(1.3)
    s = status()
    assert not s['startup'] and s['applied_percent'] == s['target_percent']
    t = s['temperature_c']
    s = configure(stop_c=round(t-4, 1), start_c=round(t+4, 1), full_c=round(t+14, 1))
    assert s['target_percent'] == 30 and s['auto_running']
    configure(auto_enabled=0)
    s = configure(auto_enabled=1)
    assert s['target_percent'] == 0 and not s['auto_running']
    print('Speed ramp, restart boost, and hysteresis ON/OFF transitions OK', flush=True)
    t = status()['temperature_c']
    s = configure(stop_c=round(t-20, 1), start_c=round(t-15, 1), full_c=round(t-10, 1))
    assert s['target_percent'] == 100 and s['auto_running']
    print('High-temperature full-duty response OK', flush=True)
    t = status()['temperature_c']
    curve = f'{round(t-10,1)}:0,{round(t,1)}:60,{round(t+10,1)}:100'
    s = configure(strategy='curve', curve=curve, curve_hysteresis_c=1, curve_deadband_c=2.5)
    assert s['control']['strategy'] == 'curve' and len(s['control']['curve_points']) == 3
    assert 50 <= s['target_percent'] <= 70
    assert s['control']['curve_deadband_c'] == 2.5
    assert isinstance(s['curve_reference_c'], (float, int))
    reference, duty = s['curve_reference_c'], s['target_percent']
    time.sleep(3.2)
    held = status()
    if abs(held['temperature_c'] - reference) < 2.4:
        assert held['target_percent'] == duty and held['curve_reference_c'] == reference
    before = s['control']
    for bad_curve in ['35:0', '35:0,35:100', '40:30,35:100', '35:50,40:30',
                      '35:0,40:10', '35:0,40:101', '35:0,40:100,', 'bad',
                      ','.join(f'{i}:20' for i in range(11))]:
        assert request('/api/control', {'curve': bad_curve})[0] == 400, bad_curve
        assert status()['control'] == before
    configure(curve='20:0,30:100')
    assert status()['target_percent'] == 100
    configure(curve='60:0,70:100')
    assert status()['target_percent'] == 0
    print('Curve interpolation, full-duty/cutoff, point editing, and invalid curves OK', flush=True)
    first = request('/api/history?minutes=30')[1]
    time.sleep(5.2)
    second = request('/api/history?minutes=30')[1]
    assert len(second['samples']) > len(first['samples']) or len(second['samples']) == 361
    print('Device-side history grows without an open dashboard', flush=True)
finally:
    configure(auto_enabled=0)
    code, data = request('/api/fan', {'percent': original['manual_percent']})
    assert code == 200
    restored = {key: original[key] for key in keys}
    restored['auto_enabled'] = int(original['auto_enabled'])
    restored['strategy'] = original['strategy']
    restored['curve_hysteresis_c'] = original['curve_hysteresis_c']
    restored['curve_deadband_c'] = original['curve_deadband_c']
    restored['curve'] = ','.join(f"{p['temperature_c']}:{p['percent']}" for p in original['curve_points'])
    configure(**restored)
    print('Original user control settings restored', flush=True)
