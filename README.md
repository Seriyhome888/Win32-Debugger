# Native Win32 Win32 user-mode debugger context engine featuring:

- Linear execution control transitions (si, so, out, c)
- Hardware flag control operations (Trap Flag configurations)
- Symbolic parsing resolution maps via the DbgHelp API System Reference Engine
- Persistent structural line breakpoint tracking layouts
- Live memory space manipulation modules (set, print, x hex dumps)
- Active caller evaluation backtraces (bt stack frame unwinding loops)
- PDB Expression Parsing : Evaluates raw string pointers(char*), maps multi - element local memory arrays, and handles dereferences(print* my_ptr)
- Hardware Watchpoints : Leverages CPU debug registers(DR0 / DR7) to capture memory writes natively
- Stack backtracing& Hex Dumps : Includes StackWalk64 call frame traces(bt) and 16 - byte aligned binary hex matrix representations(x)
- Execution Controls : Manages hardware trace steps(si), step - overs(so), and parent function returns(out)

  # Usage
  - Run Development Command Prompt for Visual Studio, execute run_session.bat.
  - It will compile and start sample debug session with process obtained by compiling in debug mode and running of supplied file target.c.
  - Enter 'c' to continue, 'help' to display supported commands.
  - To set up breakpoint in line enter:
    - bp source_file_name line_number
    - for example:
    - bp target.c 5
  - To display memory dump :
    - query your registers first using 'r' command, to grab the active stack pointer (ESP or EBP);
    - Execute the dump command 'x' directly by passing that memory registry pointer value:
	- x 0x00AFFBF8 32
  - To print variable value by its name enter:
    - print secret_value
    - or:
    - print my_array
    - or:
    - print my_ptr
  - To set variable value by variable name enter:
    - set variable_name new_value
    - for example:
    - set secret_value 5
   
<img width="3840" height="2016" alt="image" src="https://github.com/user-attachments/assets/bb765df5-7677-400e-99c2-bb57e6d8797e" />
    
<img width="3840" height="2016" alt="image" src="https://github.com/user-attachments/assets/d6df9d32-8092-4ed5-b9e3-b9bf6af330fc" />

<img width="3840" height="2016" alt="image" src="https://github.com/user-attachments/assets/bdbd7ba0-58d2-4d13-8b84-777be8bd0b4a" />
