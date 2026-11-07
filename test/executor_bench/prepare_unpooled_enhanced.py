#!/usr/bin/env python3
"""Prepare an include override from the historical infra MPSC executor (no pool)."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--infra-repo', type=Path, required=True)
parser.add_argument('--revision', default='ff08700')
parser.add_argument('--output', type=Path, required=True)
opt = parser.parse_args()
source = subprocess.check_output(['git', '-C', str(opt.infra_repo), 'show',
                                  opt.revision+':tm_kit/infra/RealTimeThreadedExecutor.hpp'], text=True)
begin = source.index('                    //Vyukov')
end = source.index('                // Synchronous callbacks', begin)
body = source[begin:end].replace('realtime_threaded_executor_detail', 'enhanced_dedicated_executor_detail')
body = body.replace('DedicatedThreadExecutor', 'EnhancedDedicatedThreadedExecutor')
body = body.replace('enhanced_dedicated_executor_detail::spinUntil', 'infra::realtime_threaded_executor_detail::spinUntil')
body = body.replace('EnhancedDedicatedThreadedExecutorConfig', 'infra::DedicatedThreadExecutorConfig')
path = opt.output/'tm_kit/basic/EnhancedDedicatedThreadedExecutor.hpp'
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text('#pragma once\n#include <tm_kit/infra/RealTimeThreadedExecutor.hpp>\n'
                'namespace dev { namespace cd606 { namespace tm { namespace basic {\n'
                'using infra::ThreadedExecOptions;\nnamespace enhanced_dedicated_executor_detail {\n'+body+'}}}}\n')
print(path)
