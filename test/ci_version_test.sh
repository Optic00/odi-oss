#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Andre Ziviani
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
lines = Path('.github/workflows/ci.yml').read_text().splitlines()
i = next(i for i, line in enumerate(lines) if line.strip() == '- name: Version for this comparison')
run = lines[i + 1].strip().removeprefix('run:').strip()
if run == '|':
    block = []
    for line in lines[i + 2:]:
        if line.strip() and len(line) - len(line.lstrip()) <= 8:
            break
        block.append(line)
    run = textwrap.dedent('\n'.join(block))
for ref, expected in [('', 'repro-dispatch'), ('main', 'main'), ('v1.16.0-beta.1', 'v1.16.0-beta.1'),
                      ('fix/306-mapper-fallback-priorities', 'fix-306-mapper-fallback-priorities'),
                      ('fix/nested/branch', 'fix-nested-branch')]:
    with tempfile.TemporaryDirectory() as directory:
        envfile = Path(directory) / 'env'
        env = dict(os.environ, GITHUB_REF_NAME=ref, GITHUB_ENV=str(envfile))
        subprocess.run(['bash', '-eu', '-c', run], env=env, check=True)
        actual = envfile.read_text().strip()
        assert actual == 'VERSION=' + expected, (ref, actual, expected)
        print('ok    reproducible CI version:', ref)
PY
