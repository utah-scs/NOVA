# Backdraft common functionalities used in experiments

import os
import sys
import subprocess
import platform


# some paths
__script_dir = os.path.dirname(os.path.abspath(__file__))
__bess_dir = os.path.abspath(os.path.join(__script_dir,
    '../'))
__bessctl_dir = os.path.join(__bess_dir, 'bessctl')
__bessctl_bin = os.path.join(__bessctl_dir, 'bessctl')
__bess_kmod_dir = os.path.join(__bess_dir, 'core/kmod')

def get_bessctl_dir():
    return __bessctl_dir

def get_bess_dir():
    return __bess_dir

def exitfn():
    procs = ["bessd"]
    for j in procs:
        os.system("sudo pkill " + j)
    
    # In cases of crashes
    # for j in procs:
    #     os.system("sudo pkill -9 " + j)


# ----------- BESS --------------

def stop_bess():
    print('Stopping bess\n')
    p = bessctl_do('daemon reset')
    p = bessctl_do('daemon stop', stdout=subprocess.PIPE)
    txt = p.stdout.decode()
    print(txt)

def read_bess_pipeline():
    print('Reading pipeline\n')
    p = bessctl_do('show pipeline', stdout=subprocess.PIPE)
    txt = p.stdout.decode()
    print(txt)

def bessctl_do(command, stdout=None, stderr=None, cpu_list=None):
    """
    Run bessctl command
    """
    cmd = '{} {}'.format(__bessctl_bin, command)
    if cpu_list is not None:
        assert isinstance(cpu_list, str)
        cmd  = 'taskset -c {} {}'.format(cpu_list, cmd)
    ret = subprocess.run(cmd, shell=True, stdout=stdout, stderr=stderr)
    return ret


def setup_bess_pipeline(pipeline_config_path):
    # Make sure bessctl daemon is down
    bessctl_do('daemon stop', stderr=subprocess.PIPE)

    # Run BESS config
    ret = bessctl_do('daemon start')
    if ret.returncode != 0:
        print('failed to start bess daemon', file=sys.stderr)
        return -1
    # Run a configuration (pipeline)
    cmd = 'daemon start -- run file {}'.format(pipeline_config_path)
    ret = bessctl_do(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print(ret.stdout.decode())
    print(ret.stderr.decode())
    return 0


def load_bess_kmod():
  cmd = './install'
  return subprocess.check_call(cmd, shell=True, cwd=__bess_kmod_dir) 


def get_port_packets(port_name):
    p = bessctl_do('show port {}'.format(port_name), subprocess.PIPE)
    txt = p.stdout.decode()
    txt = txt.strip()
    lines = txt.split('\n')
    count_line = len(lines)
    res = { 'rx': { 'packet': -1, 'byte': -1, 'drop': -1},
            'tx': {'packet': -1, 'byte': -1, 'drop': -1}}
    if count_line < 6:
        return res

    raw = lines[2].split()
    res['rx']['packet'] = int(raw[2].replace(',',''))
    res['rx']['byte'] = int(raw[4].replace(',',''))
    raw = lines[3].split() 
    res['rx']['drop'] = int(raw[1].replace(',',''))


    raw = lines[4].split()
    res['tx']['packet'] = int(raw[2].replace(',',''))
    res['tx']['byte'] = int(raw[4].replace(',',''))
    raw = lines[5].split() 
    res['tx']['drop'] = int(raw[1].replace(',',''))
    return res


def get_pfc_results(interface):
    cmd = 'ethtool -S {} | grep prio3_pause'.format(interface)
    log = subprocess.check_output(cmd, shell=True)
    log = log.decode()
    log = log.strip()
    lines = log.split('\n')
    res = {}
    for line in lines:
        line = line.strip()
        key, value = line.split(':')
        res[key] = int(value)
    return res


def delta_dict(before, after):
    res = {}
    for key, value in after.items():
        if isinstance(value, dict):
            res[key] = delta_dict(before[key], value)
        elif isinstance(value, (int, float)):
            res[key] = value - before[key]
    return res


def get_pps_from_info_log():
    """
    Go through /tmp/bessd.INFO file and find packet per second reports.
    Then organize and print them in stdout.
    """
    book = dict()
    with open('/tmp/bessd.INFO') as log_file:
        for line in log_file:
            if 'pcps' in line:
                raw = line.split()
                value = raw[5]
                name = raw[7]
                if name not in book:
                    book[name] = list()
                book[name].append(float(value))
    return book


def str_format_pps(book):
    res = []
    res.append("=== pause call per sec ===")
    for name in book:
        res.append(name)
        for i, value in enumerate(book[name]):
            res.append('{} {}'.format(i, value))
    res.append("=== pause call per sec ===")
    txt = '\n'.join(res)
    return txt


# ----------- Logger --------------
class Logger:
    def __init__(self, output=None):
        self._output_file = output

    def log(self, *args, end='\n'):
        for arg in args:
            self._output_file.write(arg)
            if end:
                self._output_file.write(end)

# ------------ eBPF ---------------
def compile_bess():
    cmd = './build.py bess --plugin dma_plugin'
    print("Compiling BESS: running {} ...".format(cmd))
    return subprocess.check_call(cmd, shell=True, cwd=__bess_dir)

# ------------ eBPF ---------------
def compile_ebpf(file_names, defines=None):
    define_flags = ""
    if defines:
        define_flags = " ".join(f"-D{k}={v}" for k, v in defines.items())

    for bpf in file_names:
        print("Compiling {} ...".format(bpf))
        if platform.uname().machine == 'aarch64':
            cmd = 'clang -I{}/deps/ubpf/vm/inc -I/usr/include/aarch64-linux-gnu/ -target bpf -O2 {} -c {} -o {}'.format(__bess_dir, define_flags, bpf, bpf[:-1] + 'o')
        else:
            cmd = 'clang -I{}/deps/ubpf/vm/inc -target bpf -O2 {} -c {} -o {}'.format(__bess_dir, define_flags, bpf, bpf[:-1] + 'o')

        try:
            subprocess.check_call(cmd, shell=True, cwd=__script_dir) 
        except subprocess.CalledProcessError as e:
            # If an error occurs, print the error message and exit with a non-zero status code
            print(f"Command '{e.cmd}' returned non-zero exit status {e.returncode}")
            exit(1)
        except OSError as e:
            # If an OS error occurs, print the error message and exit with a non-zero status code
            print(f"Execution failed: {e}")
            exit(1)
