# This script outputs relevant system environment info
# Run it with `python collect_env.py`.
import re
import subprocess
import sys
import time
import datetime
from collections import namedtuple

import torch

PY3 = sys.version_info >= (3, 0)

# System Environment Information
SystemEnv = namedtuple('SystemEnv', [
    'torch_version',
    'is_debug_build',
    'cuda_compiled_version',
    'gcc_version',
    'cmake_version',
    'os',
    'python_version',
    'is_cuda_available',
    'cuda_runtime_version',
    'nvidia_driver_version',
    'nvidia_gpu_models',
    'cudnn_version',
    'pip_version',  # 'pip' or 'pip3'
    'pip_packages',
    'conda_packages',
])


def run(command):
    """Returns (return-code, stdout, stderr)"""
    p = subprocess.Popen(command, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, shell=True)
    output, err = p.communicate()
    rc = p.returncode
    if PY3:
        output = output.decode("ascii")
        err = err.decode("ascii")
    return rc, output.strip(), err.strip()


def run_and_read_all(run_lambda, command):
    """Runs command using run_lambda; reads and returns entire output if rc is 0"""
    rc, out, _ = run_lambda(command)
    if rc is not 0:
        return None
    return out


def run_and_parse_first_match(run_lambda, command, regex):
    """Runs command using run_lambda, returns the first regex match if it exists"""
    rc, out, _ = run_lambda(command)
    if rc is not 0:
        return None
    match = re.search(regex, out)
    if match is None:
        return None
    return match.group(1)


def get_conda_packages(run_lambda):
    return run_and_read_all(run_lambda, 'conda list | grep "pytorch\|soumith"')


def get_gcc_version(run_lambda):
    return run_and_parse_first_match(run_lambda, 'gcc --version', r'gcc (.*)')


def get_cmake_version(run_lambda):
    return run_and_parse_first_match(run_lambda, 'cmake --version', r'cmake (.*)')


def get_nvidia_driver_version(run_lambda):
    return run_and_parse_first_match(run_lambda, 'nvidia-smi', r'Driver Version: (.*?) ')


def get_gpu_info(run_lambda):
    rc, out, _ = run_lambda('nvidia-smi -L')
    if rc is not 0:
        return None
    return out


def get_running_cuda_version(run_lambda):
    return run_and_parse_first_match(run_lambda, 'nvcc --version', r'V(.*)$')


def get_cudnn_version(run_lambda):
    """This will return a list of libcudnn.so; it's hard to tell which one is being used"""
    rc, out, _ = run_lambda('find /usr/local -type f -name "libcudnn*"  2>/dev/null')
    # find will return 1 if there are permission errors or if not found
    if len(out.strip()) == 0:
        return None
    if rc != 1 and rc != 0:
        return None
    return 'Probably one of the following: \n{}'.format(out)


def get_uname(run_lambda):
    return run_and_read_all(run_lambda, 'uname -s')


def get_mac_version(run_lambda):
    return run_and_parse_first_match(run_lambda, 'sw_vers -productVersion', r'(.*)')


def get_windows_version(run_lambda):
    return run_and_read_all(run_lambda, 'wmic os get Caption | findstr /v Caption')


def get_lsb_version(run_lambda):
    return run_and_parse_first_match(run_lambda, 'lsb_release -a', r'Description:\t(.*)')


def check_release_file(run_lambda):
    return run_and_parse_first_match(run_lambda, 'cat /etc/*-release',
                                     r'PRETTY_NAME="(.*)"')


def get_os(run_lambda):
    uname = get_uname(run_lambda)

    # No uname is a sign of no Linux. Try to get a windows version.
    if uname is None:
        return get_windows_version(run_lambda)

    if uname == 'Darwin':
        version = get_mac_version(run_lambda)
        if version is None:
            return None
        return 'Mac OSX {}'.format(version)

    if uname == 'Linux':
        # Ubuntu/Debian based
        desc = get_lsb_version(run_lambda)
        if desc is not None:
            return desc

        # Try reading /etc/*-release
        desc = check_release_file(run_lambda)
        if desc is not None:
            return desc

        return uname

    # Unknown uname
    return None


def get_pip_packages(run_lambda):
    # People generally have `pip` as `pip` or `pip3`
    def run_with_pip(pip):
        return run_and_read_all(run_lambda, pip + ' list --format=legacy | grep "torch\|numpy"')

    if not PY3:
        return 'pip', run_with_pip('pip')

    # Try to figure out if the user is running pip or pip3.
    out2 = run_with_pip('pip')
    out3 = run_with_pip('pip3')

    num_pips = len([x for x in [out2, out3] if x is not None])
    if num_pips is 0:
        return 'pip', out2

    if num_pips == 1:
        if out2 is not None:
            return 'pip', out2
        return 'pip3', out3

    # num_pips is 2. Return pip3 by default b/c that most likely
    # is the one associated with Python 3
    return 'pip3', out3


def get_env_info():
    run_lambda = run
    pip_version, pip_list_output = get_pip_packages(run_lambda)

    return SystemEnv(
        torch_version=torch.__version__,
        is_debug_build=torch.version.debug,
        python_version='{}.{}'.format(sys.version_info[0], sys.version_info[1]),
        is_cuda_available=torch.cuda.is_available(),
        cuda_compiled_version=torch.version.cuda,
        cuda_runtime_version=get_running_cuda_version(run_lambda),
        nvidia_gpu_models=get_gpu_info(run_lambda),
        nvidia_driver_version=get_nvidia_driver_version(run_lambda),
        cudnn_version=get_cudnn_version(run_lambda),
        pip_version=pip_version,
        pip_packages=pip_list_output,
        conda_packages=get_conda_packages(run_lambda),
        os=get_os(run_lambda),
        gcc_version=get_gcc_version(run_lambda),
        cmake_version=get_cmake_version(run_lambda),
    )

env_info_fmt = """
PyTorch version: {torch_version}
Debug build: {is_debug_build}
PyTorch compiled with CUDA: {cuda_compiled_version}
How you installed PyTorch: Unknown

OS: {os}
GCC version: {gcc_version}
CMake version: {cmake_version}

Python version: {python_version}
CUDA available: {is_cuda_available}
CUDA runtime version: {cuda_runtime_version}
GPU models and configuration:
{nvidia_gpu_models}

Nvidia driver version: {nvidia_driver_version}
cuDNN version: {cudnn_version}

Versions of relevant libraries:
{pip_packages}
{conda_packages}
""".strip()


def pretty_str(envinfo):
    def replace_nones(dct, replacement='Unknown'):
        for key in dct.keys():
            if dct[key] is not None:
                continue
            dct[key] = replacement
        return dct

    def prepend(text, tag='[prepend]'):
        lines = text.split('\n')
        updated_lines = [tag + line for line in lines]
        return '\n'.join(updated_lines)

    mutable_dict = envinfo._asdict()
    if mutable_dict['pip_packages'] is not None:
        mutable_dict['pip_packages'] = prepend(mutable_dict['pip_packages'],
                                               '[{}] '.format(envinfo.pip_version))
    if mutable_dict['conda_packages'] is not None:
        mutable_dict['conda_packages'] = prepend(mutable_dict['conda_packages'],
                                                 '[conda] ')

    # If the machine doesn't have CUDA, report some fields as 'Unavailable'
    dynamic_cuda_fields = [
        'cuda_runtime_version',
        'nvidia_gpu_models',
        'nvidia_driver_version',
    ]
    all_cuda_fields = list(dynamic_cuda_fields)
    all_cuda_fields.append('cudnn_version')

    no_dynamic_cuda = all(mutable_dict[field] is None for field in dynamic_cuda_fields)
    if not torch.cuda.is_available() and no_dynamic_cuda:
        for field in all_cuda_fields:
            mutable_dict[field] = 'Unavailable'
        if envinfo.cuda_compiled_version is None:
            mutable_dict['cuda_compiled_version'] = 'None'

    mutable_dict = replace_nones(mutable_dict)
    return env_info_fmt.format(**mutable_dict)


def get_pretty_env_info():
    return pretty_str(get_env_info())


def main():
    timestamp = datetime.datetime.fromtimestamp(time.time()).strftime('%Y-%m-%d--%H-%M-%S')
    outfile = 'env-{}.out'.format(timestamp)
    print("Collecting environment information...")
    output = get_pretty_env_info()
    with open(outfile, 'a') as f:
        f.write(output)
    print('Done. Summary written to {}'.format(outfile))


if __name__ == '__main__':
    main()
