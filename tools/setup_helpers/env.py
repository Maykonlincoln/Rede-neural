import os
import platform
import sys
from itertools import chain


IS_WINDOWS = (platform.system() == 'Windows')
IS_DARWIN = (platform.system() == 'Darwin')
IS_LINUX = (platform.system() == 'Linux')


IS_CONDA = 'conda' in sys.version or 'Continuum' in sys.version or any([x.startswith('CONDA') for x in os.environ])
CONDA_DIR = os.path.join(os.path.dirname(sys.executable), '..')


def check_env_flag(name, default=''):
    return os.getenv(name, default).upper() in ['ON', '1', 'YES', 'TRUE', 'Y']


def gather_paths(env_vars):
    return list(chain(*(os.getenv(v, '').split(':') for v in env_vars)))
