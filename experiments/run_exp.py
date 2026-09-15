#!/usr/bin/python3


import argparse
import sys
from time import sleep 
sys.path.insert(0, "../")
from naam_common import *

cur_script_dir = os.path.dirname(os.path.abspath(__file__))

def main():
    """
    About experiment:

    Run NAAM experimets
    """
    sleep(1)

    # make sure our processes are stopped
    exitfn()

    if build_bess:
        # compiling bess
        ret = compile_bess()
        if ret != 0:
            print("Failed to compile bess")
            return 1

    # compile ebpf
    if len(bpf_files_path) != 0:
        compile_ebpf(bpf_files_path, defines=ebpf_defines)

    # Find abs path from relative path
    file_path = os.path.abspath(conf_file)

    # BPF object file
    bpf_obj = ""
    i = 0
    if len(bpf_files_path) != 0:
        for bpf_file in bpf_files_path:
            bpf_obj += "bpf_obj{}=".format(i)
            bpf_obj += "\"" + bpf_file.rsplit(sep='.')[0] + ".o" + "\""
            bpf_obj += ", "
            i += 1
    else:
        bpf_obj = "bpf_obj=\"dummy\", "

    num_keys_env = "num_keys={}, ".format(num_keys) if num_keys > 0 else ""
    print('Starting bess daemon and loading pipeline...')
    if enable_jit:
        ret = bessctl_do('daemon start {} -- run file {} \'enable_jit=True, {}{}nobj={}, ncpu={}\''.format(bessd_option, file_path, num_keys_env, bpf_obj, len(bpf_files_path), ncpu))
    else:
        ret = bessctl_do('daemon start {} -- run file {} \'enable_jit=False, {}{}nobj={}, ncpu={}\''.format(bessd_option, file_path, num_keys_env, bpf_obj, len(bpf_files_path), ncpu))

    # If experiment run time is provided
    # then run the experiment for that amount of time
    # show the pipeline and exit
    # Otherwise run bessd in the background indefinitely
    if exp_time > 0:
        sleep(exp_time)
        read_bess_pipeline()
        stop_bess()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Run NAAM experiments')
    parser.add_argument('-e','--exp-dir', type=str, help='Provide an experiment directory')
    parser.add_argument('-c','--config-file', type=str, required=True, help='Provide a bess configuration file (Path relative to experiment directory)')
    parser.add_argument('-b','--bpf-file', type=str, action='append', help='Provide bpf file(s) to compile (Path relative to experiment directory)')
    parser.add_argument('-m','--build-bess', action='store_true', help='Build bess before running experiment')
    parser.add_argument('-t','--run-for', type=int, default=0, help='Run the experiment for RUN_FOR seconds. Run indefinitely if ommited or <= 0')
    parser.add_argument('-j','--jit', action='store_true', help='JIT compile the BPF program')
    parser.add_argument('-n', '--ncpu', type=int, default=1, help='Number of CPU cores to run the pipeline on')
    parser.add_argument('-o', '--bessd-option', nargs='+', type=str, action='append', help='bessd options')
    parser.add_argument('--num-keys', type=int, default=0,
        help='Number of KV pairs (0 = use default from naam.h)')

    args = parser.parse_args()
    exp_dir = args.exp_dir
    conf_file = args.config_file
    bpf_file = args.bpf_file
    exp_time = args.run_for
    build_bess = args.build_bess
    enable_jit = args.jit
    ncpu = args.ncpu
    num_keys = args.num_keys
    bessd_option = ""
    if args.bessd_option:
        bessd_option = args.bessd_option[0][0]

    # Compute eBPF -D defines when num_keys is overridden
    ebpf_defines = {}
    if num_keys > 0:
        import math
        num_buckets_cli = 1 << math.ceil(math.log2(max(num_keys // 15, 1)))
        key_num_bits_cli = math.ceil(math.log2(num_keys))
        ebpf_defines = {
            'NUM_KEYS':     num_keys,
            'NUM_BUCKETS':  num_buckets_cli,
            'KEY_NUM_BITS': key_num_bits_cli,
        }
    
    # Check if files exists and if yes, the convert to absolute path
    bpf_files_path = []
    if bpf_file != None:
        for f in bpf_file:
            if not os.path.exists(f):
                print("BPF file [%s] doesn't exist!" % f)
                exit(1)
            f = os.path.abspath(f)
            bpf_files_path.append(f)

    if not os.path.exists(conf_file):
        print("Configuration file [%s] doesn't exist!" % conf_file)
        exit(1)

    conf_file = os.path.abspath(conf_file)

    main()
