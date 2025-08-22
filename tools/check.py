import subprocess
import filecmp
import sys
import os

def compile_cpp(source_file, executable_path):
    print(f"Compiling {source_file}...")
    compile_command = ['g++', '-std=c++23', '-O2', '-o', executable_path, source_file]
    try:
        subprocess.run(compile_command, check=True, capture_output=True, text=True)
        print(f"Successfully compiled {source_file}.")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error compiling {source_file}:")
        print(e.stderr)
        return False

def run_simulator(executable_path, input_file, output_file):
    print(f"Running {executable_path} with {input_file}...")
    
    run_command = [executable_path, input_file]
    
    try:
        with open(output_file, 'w', newline='\n') as f_out:
            subprocess.run(run_command, check=True, stdout=f_out, stderr=subprocess.PIPE, text=True)
        print(f"Finished running {executable_path}. Output saved to {output_file}.")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error running {executable_path}:")
        print(e.stderr)
        return False
    except FileNotFoundError:
        print(f"Error: Executable '{executable_path}' not found. Did compilation fail?")
        return False

def main():
    if len(sys.argv) < 2:
        print(f"Usage: python {sys.argv[0]} <input_file>")
        sys.exit(1)

    input_file = sys.argv[1]

    stdlist_source = 'map_stdlist.cpp'
    flatvec_source = 'flatvec_instrusive.cpp'
    mapLL_source = 'map_LL_intrusive.cpp'

    stdlist_exe = 'stdlist'
    flatvec_exe = 'flatvec'
    mapLL_exe = 'mapLL'
    
    if os.name == 'nt':
        stdlist_exe_path = f'.\\{stdlist_exe}.exe'
        flatvec_exe_path = f'.\\{flatvec_exe}.exe'
        mapLL_exe_path = f'.\\{mapLL_exe}.exe'
    else:
        sim_exe_path = f'./{sim_exe}'
        flatvec_exe_path = f'./{flatvec_exe}'
        mapLL_exe_path =f'./{mapLL_exe}'

    stdlist_output = 'sim_output.txt'
    flatvec_output = 'flatvec_output.txt'
    mapll_output = 'mapll_output.txt'
    # compile
    if not compile_cpp(stdlist_source, stdlist_exe_path):
        sys.exit(1)
    if not compile_cpp(flatvec_source, flatvec_exe_path):
        sys.exit(1)
    if not compile_cpp(mapLL_source, mapLL_exe_path):
        sys.exit(1)

    #run
    if not run_simulator(stdlist_exe_path, input_file, stdlist_output):
        sys.exit(1)
    if not run_simulator(flatvec_exe_path, input_file, flatvec_output):
        sys.exit(1)
    if  not run_simulator(mapLL_exe_path, input_file, mapll_output ):
        sys.exit(1)
        
  
    print("\nComparing outputs...")
    if filecmp.cmp(stdlist_output, flatvec_output, shallow=False):
        print("Outputs 1+2 are identical")
        
    if filecmp.cmp(mapll_output, flatvec_output, shallow=False):
        print("++ Success ; Outputs 2+3 are identical")
    else:
        print("-- Failure!- Outputs differ")
        print("You can compare the files with a diff tool, e.g.:")
        print(f"fc {stdlist_output} {flatvec_output} {mapll_output}")

if __name__ == "__main__":
    main()
