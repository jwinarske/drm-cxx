#!/usr/bin/env python3
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: MIT
"""Scan a drmdb snapshot for compatibility with drm-cxx examples.

drmdb (https://drmdb.emersion.fr/) collects drm_info JSON dumps from
real hardware. The snapshot tarball contains one .json per probed
device. This script classifies every entry against the capability
requirements of each drm-cxx example and prints a summary suitable for
CI consumption.

Usage:
    drmdb_compat_scan.py [--input DIR] [--results-json PATH]

Defaults: --input ./drmdb-data, --results-json scan_results.json.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from collections import Counter

# Connector status: 1=connected, 2=disconnected, 3=unknown
CONNECTED = 1

# Plane "type" property values: spec list resolves name → numeric id; the
# JSON's spec list reorders by id so we resolve by name rather than
# trusting the index.

FOURCC = {
    'XRGB8888': 0x34325258,  # 'XR24'
    'ARGB8888': 0x34325241,  # 'AR24'
    'XBGR8888': 0x34324258,
    'ABGR8888': 0x34324241,
    'XRGB2101010': 0x30335258,
    'NV12': 0x3231564E,
    'YUYV': 0x56595559,
}


def plane_type_name(plane):
    p = plane['properties'].get('type')
    if not p:
        return None
    spec = p.get('spec') or []
    raw = p.get('raw_value')
    for s in spec:
        if s['value'] == raw:
            return s['name'].upper()
    return None


def plane_has(plane, prop):
    return plane['properties'].get(prop) is not None


def plane_supports_format(plane, fourcc):
    fmts = plane.get('formats') or []
    return fourcc in fmts


def classify(d):
    drv = d['driver']
    name = drv['name']
    caps = drv.get('caps') or {}
    cc = drv.get('client_caps') or {}
    connectors = d.get('connectors') or []
    crtcs = d.get('crtcs') or []
    planes = d.get('planes') or []

    has_atomic = bool(cc.get('ATOMIC'))
    has_universal = bool(cc.get('UNIVERSAL_PLANES'))
    has_writeback = bool(cc.get('WRITEBACK_CONNECTORS'))
    has_dumb = bool(caps.get('DUMB_BUFFER'))
    prime = caps.get('PRIME', 0) or 0
    has_prime = (prime & 1) != 0
    has_modifiers = bool(caps.get('ADDFB2_MODIFIERS'))
    cursor_w = caps.get('CURSOR_WIDTH', 0) or 0
    cursor_h = caps.get('CURSOR_HEIGHT', 0) or 0

    has_kms = len(crtcs) > 0 and len(planes) > 0
    n_conn = sum(1 for c in connectors if c.get('status') == CONNECTED)

    plane_types = Counter()
    primary_xrgb = primary_argb = 0
    overlay_argb = overlay_xrgb = 0
    cursor_argb = 0
    zpos_present = 0
    in_formats_present = 0
    primary_planes = []
    overlay_planes = []
    cursor_planes = []

    for p in planes:
        t = plane_type_name(p)
        if t:
            plane_types[t] += 1
        if plane_has(p, 'zpos'):
            zpos_present += 1
        if plane_has(p, 'IN_FORMATS'):
            in_formats_present += 1
        if t == 'PRIMARY':
            primary_planes.append(p)
            if plane_supports_format(p, FOURCC['XRGB8888']):
                primary_xrgb += 1
            if plane_supports_format(p, FOURCC['ARGB8888']):
                primary_argb += 1
        elif t == 'OVERLAY':
            overlay_planes.append(p)
            if plane_supports_format(p, FOURCC['ARGB8888']):
                overlay_argb += 1
            if plane_supports_format(p, FOURCC['XRGB8888']):
                overlay_xrgb += 1
        elif t == 'CURSOR':
            cursor_planes.append(p)
            if plane_supports_format(p, FOURCC['ARGB8888']):
                cursor_argb += 1

    n_overlay = plane_types.get('OVERLAY', 0)
    n_primary = plane_types.get('PRIMARY', 0)
    n_cursor = plane_types.get('CURSOR', 0)

    # ---- Per-example compatibility ----------------------------------------
    issues = {}

    def base_kms():
        r = []
        if not has_kms:
            r.append('no-kms (render-only or no planes/crtcs)')
            return r
        if not has_atomic:
            r.append('atomic client cap unsupported')
        if not has_universal:
            r.append('universal_planes client cap unsupported')
        if n_conn == 0:
            r.append('no connected output')
        if n_primary == 0:
            r.append('no PRIMARY plane')
        return r

    issues['atomic_modeset'] = base_kms()
    if has_kms and not has_dumb:
        issues['atomic_modeset'].append('no DUMB_BUFFER cap')
    if has_kms and primary_xrgb == 0:
        issues['atomic_modeset'].append('no PRIMARY supports XRGB8888')

    issues['overlay_planes'] = base_kms()
    if has_kms and n_overlay == 0:
        issues['overlay_planes'].append('no OVERLAY plane (example becomes empty)')

    # cursor_rotate: hardware rotation is optional (CPU pre-rotate
    # fallback in src/cursor/renderer.cpp).
    issues['cursor_rotate'] = base_kms()
    if has_kms:
        if cursor_w == 0 or cursor_h == 0:
            issues['cursor_rotate'].append('no DRM_CAP_CURSOR_WIDTH/HEIGHT')
        if n_cursor == 0:
            issues['cursor_rotate'].append('no CURSOR plane')
        elif cursor_argb == 0:
            issues['cursor_rotate'].append('CURSOR plane lacks ARGB8888')

    issues['mouse_cursor'] = base_kms()
    if has_kms:
        if cursor_w == 0 or cursor_h == 0:
            issues['mouse_cursor'].append('no DRM_CAP_CURSOR_WIDTH/HEIGHT')
        if n_cursor == 0 and overlay_argb == 0:
            issues['mouse_cursor'].append('no CURSOR plane and no OVERLAY ARGB8888 fallback')

    for ex in ('signage_player', 'layered_demo', 'video_grid',
               'scene_warm_start', 'scene_priority', 'scene_formats',
               'hotplug_monitor', 'test_patterns'):
        issues[ex] = base_kms()
        if not has_kms:
            continue
        if not has_dumb:
            issues[ex].append('no DUMB_BUFFER cap')
        if n_overlay == 0:
            issues[ex].append('no OVERLAY plane')
        elif overlay_argb == 0:
            issues[ex].append('no OVERLAY supports ARGB8888')
        if zpos_present == 0:
            issues[ex].append('no plane exposes zpos property')
        if primary_xrgb == 0:
            issues[ex].append('no PRIMARY supports XRGB8888')

    if 'signage_player' in issues and has_kms and not has_prime:
        issues['signage_player'].append('no PRIME (GBM bg fails)')

    if 'video_grid' in issues and has_kms and 0 < n_overlay < 3:
        issues['video_grid'].append(f'only {n_overlay} OVERLAY plane(s); demo wants 3+')

    if 'scene_formats' in issues and has_kms and (n_primary + n_overlay) < 4:
        issues['scene_formats'].append(
            f'only {n_primary + n_overlay} PRIMARY+OVERLAY planes; example wants 4+')
    if 'scene_priority' in issues and has_kms and n_overlay < 3:
        issues['scene_priority'].append(
            f'only {n_overlay} OVERLAY plane(s); example wants 3+')

    issues['capture_demo'] = base_kms()
    if has_kms:
        if not has_writeback:
            issues['capture_demo'].append('no WRITEBACK_CONNECTORS client cap')
        if not has_prime:
            issues['capture_demo'].append('no PRIME (dma-buf export)')

    issues['vulkan_display'] = base_kms()
    if has_kms:
        if not has_prime:
            issues['vulkan_display'].append('no PRIME (dma-buf import)')
        if primary_xrgb == 0 and primary_argb == 0:
            issues['vulkan_display'].append('PRIMARY lacks XRGB/ARGB8888')

    cursor_hw_rotation = (n_cursor > 0 and
                          any(plane_has(p, 'rotation') for p in cursor_planes))
    return {
        'driver': name,
        'kernel': drv.get('kernel', {}).get('release'),
        'has_kms': has_kms,
        'n_connected': n_conn,
        'n_primary': n_primary,
        'n_overlay': n_overlay,
        'n_cursor': n_cursor,
        'has_atomic': has_atomic,
        'has_universal': has_universal,
        'has_dumb': has_dumb,
        'has_prime': has_prime,
        'has_modifiers': has_modifiers,
        'has_writeback': has_writeback,
        'has_zpos_any': zpos_present > 0,
        'has_in_formats_any': in_formats_present > 0,
        'overlay_argb': overlay_argb,
        'primary_xrgb': primary_xrgb,
        'cursor_argb': cursor_argb,
        'cursor_hw_rotation': cursor_hw_rotation,
        'issues': issues,
    }


EXAMPLES = [
    'atomic_modeset', 'overlay_planes', 'mouse_cursor', 'cursor_rotate',
    'signage_player', 'layered_demo', 'video_grid',
    'scene_warm_start', 'scene_priority', 'scene_formats',
    'hotplug_monitor', 'test_patterns',
    'capture_demo', 'vulkan_display',
]


def aggregate(results, total):
    print(f'\n=== Total drm_info entries scanned: {total} ===\n')

    drv_counts = Counter(r['driver'] for r in results)
    print('Driver distribution (top 20):')
    for k, v in drv_counts.most_common(20):
        print(f'  {k:24s} {v:4d}  ({100*v/total:.1f}%)')

    no_kms = [r for r in results if not r['has_kms']]
    no_atomic = [r for r in results if r['has_kms'] and not r['has_atomic']]
    no_universal = [r for r in results if r['has_kms'] and not r['has_universal']]
    no_dumb = [r for r in results if r['has_kms'] and not r['has_dumb']]
    no_prime = [r for r in results if r['has_kms'] and not r['has_prime']]
    no_writeback = [r for r in results if r['has_kms'] and not r['has_writeback']]
    no_modifiers = [r for r in results if r['has_kms'] and not r['has_modifiers']]
    no_zpos = [r for r in results if r['has_kms'] and not r['has_zpos_any']]
    no_overlay = [r for r in results if r['has_kms'] and r['n_overlay'] == 0]
    no_cursor = [r for r in results if r['has_kms'] and r['n_cursor'] == 0]
    no_connected = [r for r in results if r['has_kms'] and r['n_connected'] == 0]

    def pct(rs):
        return f'{len(rs):4d} ({100*len(rs)/total:.1f}%)'

    print('\nGlobal capability gaps:')
    print(f'  no KMS at all:                            {pct(no_kms)}')
    print(f'  no ATOMIC client cap:                     {pct(no_atomic)}')
    print(f'  no UNIVERSAL_PLANES client cap:           {pct(no_universal)}')
    print(f'  no DUMB_BUFFER cap:                       {pct(no_dumb)}')
    print(f'  no PRIME cap:                             {pct(no_prime)}')
    print(f'  no WRITEBACK_CONNECTORS client cap:       {pct(no_writeback)}')
    print(f'  no ADDFB2_MODIFIERS cap:                  {pct(no_modifiers)}')
    print(f'  no plane with zpos property:              {pct(no_zpos)}')
    print(f'  no OVERLAY plane:                         {pct(no_overlay)}')
    print(f'  no CURSOR plane:                          {pct(no_cursor)}')
    print(f'  no CONNECTED output (probe-time):         {pct(no_connected)}')

    print('\nPer-example incompatibility:')
    for ex in EXAMPLES:
        bad = [r for r in results if r['issues'].get(ex)]
        print(f'  {ex:18s} fails on {len(bad):4d}/{total}  ({100*len(bad)/total:.1f}%)')

    print('\nTop failure reasons per example:')
    for ex in EXAMPLES:
        reason_count = Counter()
        for r in results:
            for reason in r['issues'].get(ex, []):
                reason_count[reason] += 1
        if not reason_count:
            print(f'\n  [{ex}] no failures')
            continue
        print(f'\n  [{ex}]')
        for reason, n in reason_count.most_common():
            print(f'     {n:4d}  {reason}')

    print('\nKey driver classes — atomic/universal/zpos/overlay availability:')
    interesting = ['amdgpu', 'i915', 'xe', 'nouveau', 'nvidia-drm',
                   'msm', 'rockchip', 'sun4i-drm', 'mediatek', 'meson',
                   'imx-drm', 'imx-dcss', 'rcar-du', 'tegra', 'exynos',
                   'tilcdc', 'tidss', 'omapdrm', 'vc4', 'virtio_gpu',
                   'vmwgfx', 'simpledrm', 'vkms', 'ast', 'evdi',
                   'mgag200', 'qxl', 'radeon', 'gma500']
    for k in sorted(set(drv_counts) & set(interesting)):
        rs = [r for r in results if r['driver'] == k]
        n = len(rs)
        atomic = sum(1 for r in rs if r['has_atomic'])
        zpos = sum(1 for r in rs if r['has_zpos_any'])
        overlay = sum(1 for r in rs if r['n_overlay'] > 0)
        cursor = sum(1 for r in rs if r['n_cursor'] > 0)
        modifiers = sum(1 for r in rs if r['has_modifiers'])
        prime = sum(1 for r in rs if r['has_prime'])
        wb = sum(1 for r in rs if r['has_writeback'])
        univ = sum(1 for r in rs if r['has_universal'])
        print(f'  {k:18s} N={n:3d}  atomic={atomic:3d} univ={univ:3d} '
              f'zpos={zpos:3d} ov={overlay:3d} cur={cursor:3d} mod={modifiers:3d} '
              f'prime={prime:3d} wb={wb:3d}')

    fully_compatible = [r for r in results
                        if all(not r['issues'].get(ex) for ex in EXAMPLES)]
    pass_floor = sum(1 for r in results if not r['issues']['atomic_modeset'])
    print(f'\nDevices passing ALL examples: {len(fully_compatible)}/{total}')
    print(f'Devices passing atomic_modeset (the floor): {pass_floor}/{total}')

    cursor_kms = [r for r in results if r['has_kms'] and r['n_cursor'] > 0]
    cursor_no_hw_rot = [r for r in cursor_kms if not r['cursor_hw_rotation']]
    print('\nSoft notes:')
    print(f'  cursor_rotate runs everywhere with a CURSOR plane; '
          f'CPU pre-rotate fallback engages on {len(cursor_no_hw_rot)}/'
          f'{len(cursor_kms)} cursor-capable devices')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--input', default='./drmdb-data',
                    help='Directory containing drmdb snapshot .json files')
    ap.add_argument('--results-json', default='scan_results.json',
                    help='Where to write per-entry results (machine-readable)')
    args = ap.parse_args()

    pattern = os.path.join(args.input, '*.json')
    files = [f for f in sorted(glob.glob(pattern))
             if os.path.basename(f) != os.path.basename(args.results_json)]
    if not files:
        print(f'no .json files found in {args.input}', file=sys.stderr)
        return 2

    results = []
    for f in files:
        try:
            d = json.load(open(f))
        except Exception as e:
            print(f'parse error {f}: {e}', file=sys.stderr)
            continue
        r = classify(d)
        r['_file'] = os.path.basename(f)
        results.append(r)

    aggregate(results, len(results))

    with open(args.results_json, 'w') as fh:
        json.dump(results, fh, indent=2, default=str)
    print(f'\nFull per-entry results: {args.results_json}')
    return 0


if __name__ == '__main__':
    sys.exit(main())