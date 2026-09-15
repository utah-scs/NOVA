import subprocess
import os.path
import argparse
import platform

# Script directory
script_dir = os.path.dirname(os.path.realpath(__file__))

# Test and respective test pkt
tests = [
            ("test-noop.c", "read.pkt"),
            ("test-print.c", "read.pkt"),
            ("test-yield.c", "read.pkt"),
            ("test-ptr-pkt.c", "read.pkt"),
            ("test-ptr-stack.c", "read.pkt"),
            ("test-read.c", "read.pkt"),
            ("test-write.c", "write.pkt"),
            ("test-read-write.c", "read.pkt"),
            ("test-read-write.c", "write.pkt"),
            ("test-read-write-verify.c", "read.pkt"),
            ("test-cas.c", "read.pkt"),
            ("test-faa.c", "read.pkt"),
        ]

def run_tests():
    # Compile all test files
    print("Compiling all tests...")
    for test in tests:
        f_name = script_dir + "/" + test[0]
        f_name_no_ext = f_name.split(".")[0]

        if platform.processor() == "aarch64":
            cmd = ["clang", "-Wall", "-g", "-O2", "-target", "bpf",
                   "-I", script_dir + "/../inc/", "-I", "/usr/include/aarch64-linux-gnu/", "-o" , f_name_no_ext + ".o", "-c", f_name]
        else:
            cmd = ["clang", "-Wall", "-g", "-O2", "-target", "bpf", "-I", script_dir + "/../inc/", "-o" , f_name_no_ext + ".o", "-c", f_name]

        sp = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

        if sp.returncode != 0:
            print("Error compiling " + test[0])
            exit(1)

    # Generate packet for test
    print("Generating packets for tests...")
    sp = subprocess.run([script_dir + "/pktgen.py"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if sp.returncode != 0:
        print("Error generating packets")
        exit(1)

    if jit_enabled:
        print("Running all tests with JIT enabled...")
    else:
        print("Running all tests with JIT disabled...")

    # Run all tests
    for test in tests:
        print(test[0] + ": ", end='')
        f_name = script_dir + "/" + test[0]
        f_name_no_ext = f_name.split(".")[0]
        f_name_obj = f_name_no_ext + ".o"
        if jit_enabled:
            cmd = [script_dir + "/../test", "-j", "-m", script_dir + "/" + test[1], f_name_obj]
        else:
            cmd = [script_dir + "/../test", "-m", script_dir + "/" + test[1], f_name_obj]
        sp = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        if sp.returncode != 0:
            print("FAILED")
        else:
            print("PASSED")

def clean_tests():
    print("Cleaning tests...")
    cmd = "rm -f " + script_dir + "/*.o " + script_dir + "/*.pkt"
    sp = subprocess.Popen(cmd, shell=True)
    sp.wait()

    # Check if command was successful
    if sp.returncode != 0:
        print("Error cleaning tests")
        exit(1)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run tests')
    cmds = {"run": run_tests, "clean": clean_tests}
    cmdlist = sorted(cmds.keys())
    
    parser.add_argument(
        'action',
        metavar='action',
        nargs='?',
        default='run',
        choices=cmdlist,
        help='Action is one of ' + '[' + ', '.join(cmdlist) + ']')
    
    parser.add_argument(
        '-j', '--jit',
        action='store_true',
        help='Run tests with JIT enabled')
    
    args = parser.parse_args()
    jit_enabled = args.jit
    
    cmds[args.action]()
