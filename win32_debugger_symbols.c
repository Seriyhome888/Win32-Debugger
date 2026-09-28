// win32_debugger_symbols.c
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>

#pragma comment(lib, "dbghelp.lib")
#include <dbghelp.h>

#define MAX_BREAKPOINTS 16

typedef struct {
	void* address;
	BYTE original_byte;
	bool is_set;
	bool is_temporary;
} Breakpoint;

Breakpoint g_breakpoints[MAX_BREAKPOINTS] = { 0 };
bool g_is_single_stepping = false;
bool g_symbols_initialized = false;
DWORD64 g_base_address = 0;

typedef struct {
	const char* target_name;
	DWORD64 address;
	bool found;
} FindVariableContext;

BOOL CALLBACK EnumSymbolsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) 
{
	FindVariableContext* context = (FindVariableContext*)UserContext;
	if (strcmp(pSymInfo->Name, context->target_name) == 0) 
	{
		context->address = pSymInfo->Address;
		context->found = true;
		return FALSE;
	}
	return TRUE;
}

bool ReadTargetMemory(HANDLE process_handle, void* address, void* buffer, size_t size) 
{
	SIZE_T bytes_read;
	return ReadProcessMemory(process_handle, address, buffer, size, &bytes_read) && (bytes_read == size);
}

bool WriteTargetMemory(HANDLE process_handle, void* address, const void* buffer, size_t size) 
{
	SIZE_T bytes_written;
	return WriteProcessMemory(process_handle, address, buffer, size, &bytes_written) && (bytes_written == size);
}

bool AddBreakpointExtended(HANDLE process_handle, void* address, bool is_temp) 
{
	for (int i = 0; i < MAX_BREAKPOINTS; i++) 
	{
		if (g_breakpoints[i].is_set && g_breakpoints[i].address == address) return true;
	}

	for (int i = 0; i < MAX_BREAKPOINTS; i++) 
	{
		if (!g_breakpoints[i].is_set) 
		{
			BYTE original;
			if (ReadTargetMemory(process_handle, address, &original, 1)) 
			{
				BYTE int3_opcode = 0xCC;
				if (WriteTargetMemory(process_handle, address, &int3_opcode, 1)) 
				{
					g_breakpoints[i].address = address;
					g_breakpoints[i].original_byte = original;
					g_breakpoints[i].is_set = true;
					g_breakpoints[i].is_temporary = is_temp;
					return true;
				}
			}
			break;
		}
	}
	return false;
}

bool AddBreakpoint(HANDLE process_handle, void* address) 
{
	return AddBreakpointExtended(process_handle, address, false);
}

void SetSingleStepTrap(HANDLE thread_handle, bool enable) 
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL;
	GetThreadContext(thread_handle, &ctx);
	if (enable) ctx.EFlags |= 0x100;
	else ctx.EFlags &= ~0x100;
	SetThreadContext(thread_handle, &ctx);
}

bool ResolveVariableAddress(HANDLE process_handle, HANDLE thread_handle, const char* var_name, DWORD64* out_address) 
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	GetThreadContext(thread_handle, &ctx);

	IMAGEHLP_STACK_FRAME sf = { 0 };
#ifdef _WIN64
	sf.InstructionOffset = ctx.Rip;
	sf.FrameOffset = ctx.Rbp;
#else
	sf.InstructionOffset = ctx.Eip;
	sf.FrameOffset = ctx.Ebp;
#endif

	SymSetContext(process_handle, &sf, &ctx);

	FindVariableContext search_ctx = { var_name, 0, false };
	if (SymEnumSymbols(process_handle, 0, NULL, EnumSymbolsCallback, &search_ctx) && search_ctx.found) 
	{
		int stack_offset = (int)(search_ctx.address & 0xFFFFFFFF);
		if (search_ctx.address >= 0x8000000000000000ULL || stack_offset < 0 || search_ctx.address < 0x10000) 
		{
#ifdef _WIN64
			* out_address = ctx.Rbp + stack_offset;
#else
			* out_address = ctx.Ebp + stack_offset;
#endif
		}
		else 
		{
			*out_address = search_ctx.address;
		}
		return true;
	}
	return false;
}

void PrintVariableByName(HANDLE process_handle, HANDLE thread_handle, const char* var_name) 
{
	if (!g_symbols_initialized) return;

	DWORD64 absolute_address = 0;
	if (ResolveVariableAddress(process_handle, thread_handle, var_name, &absolute_address)) 
	{
		int value = 0;
		if (ReadTargetMemory(process_handle, (void*)absolute_address, &value, sizeof(value))) 
		{
			printf("[+] Variable '%s' (at 0x%llX) value: %d (0x%X)\n", var_name, absolute_address, value, value);
		}
		else 
		{
			printf("[-] Failed to read memory value at address 0x%llX\n", absolute_address);
		}
	}
	else 
	{
		printf("[-] Symbol variable '%s' could not be resolved in the current active local stack frame layer.\n", var_name);
	}
}

void ModifyVariableByName(HANDLE process_handle, HANDLE thread_handle, const char* var_name, int new_value) 
{
	if (!g_symbols_initialized) return;

	DWORD64 absolute_address = 0;
	if (ResolveVariableAddress(process_handle, thread_handle, var_name, &absolute_address)) 
	{
		if (WriteTargetMemory(process_handle, (void*)absolute_address, &new_value, sizeof(new_value))) 
		{
			printf("[+] Successfully patched '%s' to value: %d\n", var_name, new_value);
		}
		else 
		{
			printf("[-] Failed to write memory value to address 0x%llX\n", absolute_address);
		}
	}
	else 
	{
		printf("[-] Symbol variable '%s' could not be resolved in the current active local stack frame layer.\n", var_name);
	}
}

void SetLineBreakpoint(HANDLE process_handle, const char* filename, int line_number) 
{
	IMAGEHLP_LINE64 line_info = { 0 };
	line_info.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
	LONG displacement = 0;

	if (SymGetLineFromName64(process_handle, NULL, filename, line_number, &displacement, &line_info)) 
	{
		void* bp_address = (void*)line_info.Address;
		if (AddBreakpoint(process_handle, bp_address)) 
		{
			printf("[+] Successfully set line breakpoint at %s:%d (Address: 0x%p)\n", filename, line_number, bp_address);
		}
	}
	else 
	{
		printf("[-] Error: Could not resolve line %d in file '%s'.\n", line_number, filename);
	}
}

void HandleStepOver(HANDLE process_handle, HANDLE thread_handle) 
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL;
	GetThreadContext(thread_handle, &ctx);

	BYTE instruction_buffer[16] = { 0 };
#ifdef _WIN64
	void* current_ip = (void*)ctx.Rip;
#else
	void* current_ip = (void*)ctx.Eip;
#endif

	if (ReadTargetMemory(process_handle, current_ip, instruction_buffer, sizeof(instruction_buffer))) 
	{
		BYTE opcode = instruction_buffer[0];
		if (opcode == 0xE8) 
		{
			void* next_instruction = (void*)((BYTE*)current_ip + 5);
			AddBreakpointExtended(process_handle, next_instruction, true);
			g_is_single_stepping = false;
			return;
		}
		else if (opcode == 0xFF && (instruction_buffer[1] & 0x30) == 0x10) 
		{
			void* next_instruction = (void*)((BYTE*)current_ip + 2);
			AddBreakpointExtended(process_handle, next_instruction, true);
			g_is_single_stepping = false;
			return;
		}
	}
	g_is_single_stepping = true;
	SetSingleStepTrap(thread_handle, true);
}

void HandleStepOut(HANDLE process_handle, HANDLE thread_handle) 
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	GetThreadContext(thread_handle, &ctx);
	DWORD64 return_address = 0;

#ifdef _WIN64
	if (ReadTargetMemory(process_handle, (void*)ctx.Rsp, &return_address, sizeof(return_address)) && return_address != 0) 
	{
		AddBreakpointExtended(process_handle, (void*)return_address, true);
		g_is_single_stepping = false;
	}
#else
	DWORD32 ret_addr_32 = 0;
	void* ebp_return_ptr = (void*)(ctx.Ebp + 4);
	if (ReadTargetMemory(process_handle, ebp_return_ptr, &ret_addr_32, sizeof(ret_addr_32)) && ret_addr_32 != 0) 
	{
		return_address = ret_addr_32;
		AddBreakpointExtended(process_handle, (void*)return_address, true);
		g_is_single_stepping = false;
	}
#endif
}

void DumpMemoryHex(HANDLE process_handle, void* start_address, size_t dynamic_bytes)
{
	BYTE buffer[16];
	size_t rows = (dynamic_bytes + 15) / 16;

	printf("\n--- Memory Dump at 0x%p ---\n", start_address);
	for (size_t r = 0; r < rows; r++)
	{
		void* current_row_addr = (BYTE*)start_address + (r * 16);
		size_t bytes_to_read = (dynamic_bytes - (r * 16) < 16) ? (dynamic_bytes - (r * 16)) : 16;

		ZeroMemory(buffer, 16);
		if (!ReadTargetMemory(process_handle, current_row_addr, buffer, bytes_to_read))
		{
			printf("0x%p:  [Memory Read Failed]\n", current_row_addr);
			break;
		}

		// Print Hexadecimal Blocks
		printf("0x%p:  ", current_row_addr);
		for (size_t i = 0; i < 16; i++)
		{
			if (i < bytes_to_read) printf("%02X ", buffer[i]);
			else printf("   ");
			if (i == 7) printf(" "); // Visual anchor split
		}

		// Print ASCII Character Representation
		printf(" | ");
		for (size_t i = 0; i < bytes_to_read; i++)
		{
			char ch = (char)buffer[i];
			if (ch >= 32 && ch <= 126) printf("%c", ch);
			else printf("."); // Fallback placeholder symbol
		}
		printf("\n");
	}
}

void PrintCallStackBacktrace(HANDLE process_handle, HANDLE thread_handle)
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	GetThreadContext(thread_handle, &ctx);

	STACKFRAME64 frame = { 0 };
	DWORD machine_type = IMAGE_FILE_MACHINE_I386;

#ifdef _WIN64
	machine_type = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#else
	frame.AddrPC.Offset = ctx.Eip;
	frame.AddrFrame.Offset = ctx.Ebp;
	frame.AddrStack.Offset = ctx.Esp;
#endif

	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;

	printf("\n--- Active Call Stack Backtrace ---\n");
	int frame_depth = 0;

	while (StackWalk64(machine_type, process_handle, thread_handle, &frame, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
	{
		if (frame.AddrPC.Offset == 0) break;

		char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
		PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
		pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		pSymbol->MaxNameLen = MAX_SYM_NAME;
		DWORD64 displacement = 0;

		if (SymFromAddr(process_handle, frame.AddrPC.Offset, &displacement, pSymbol))
		{
			printf("  [%d] 0x%I64X -> %s() + 0x%I64X\n", frame_depth++, frame.AddrPC.Offset, pSymbol->Name, displacement);
		}
		else
		{
			printf("  [%d] 0x%I64X -> [Unknown Function module]\n", frame_depth++, frame.AddrPC.Offset);
		}
	}
}

void DebuggerConsolePrompt(HANDLE process_handle, HANDLE thread_handle) 
{
	char cmd[64];

	while (true) 
	{
		printf("\n[dbgr]> ");

		if (scanf_s("%63s", cmd, (unsigned int)sizeof(cmd)) <= 0) continue;

		if (strcmp(cmd, "help") == 0) 
		{
			printf("Commands:\n  r            - Print registers\n  si           - Step Into\n  so           - Step Over\n  out          - Step Out\n  c            - Continue\n  print <v>    - Print variable value\n  set <v> <n>  - Set variable value\n  bp <f> <l>   - Set line breakpoint\n");
		}
		else if (strcmp(cmd, "r") == 0) 
		{
			CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
			GetThreadContext(thread_handle, &ctx);
#ifdef _WIN64
			printf("Registers: RIP=0x%p, RSP=0x%p, RBP=0x%p\n", (void*)ctx.Rip, (void*)ctx.Rsp, (void*)ctx.Rbp);
#else
			printf("Registers: EIP=0x%08lX, ESP=0x%08lX, EBP=0x%08lX\n", ctx.Eip, ctx.Esp, ctx.Ebp);
#endif
		}
		else if (strcmp(cmd, "si") == 0) 
		{
			g_is_single_stepping = true;
			SetSingleStepTrap(thread_handle, true);
			break;
		}
		else if (strcmp(cmd, "so") == 0) 
		{
			HandleStepOver(process_handle, thread_handle);
			break;
		}
		else if (strcmp(cmd, "out") == 0) 
		{
			HandleStepOut(process_handle, thread_handle);
			break;
		}
		else if (strcmp(cmd, "c") == 0) 
		{
			g_is_single_stepping = false;
			break;
		}
		else if (strcmp(cmd, "bp") == 0) 
		{
			char file_arg[64];
			int line_arg = 0;
			if (scanf_s("%63s %d", file_arg, (unsigned int)sizeof(file_arg), &line_arg) > 0) 
			{
				SetLineBreakpoint(process_handle, file_arg, line_arg);
			}
		}
		else if (strcmp(cmd, "print") == 0) 
		{
			char var_name[64];
			if (scanf_s("%63s", var_name, (unsigned int)sizeof(var_name)) > 0) 
			{
				PrintVariableByName(process_handle, thread_handle, var_name);
			}
		}
		else if (strcmp(cmd, "set") == 0) 
		{
			char var_name[64];
			int new_val = 0; if (scanf_s("%63s %d", var_name, (unsigned int)sizeof(var_name), &new_val) > 0) 
			{
				ModifyVariableByName(process_handle, thread_handle, var_name, new_val); 
			}
		}
		else if (strcmp(cmd, "x") == 0) 
		{
			/*
				When you hit your breakpoint inside some function,
				query your registers first using r to grab the active stack pointer (ESP or EBP).
				Execute the dump command directly by passing that memory registry pointer value:
				[dbgr]> x 0x00AFFBF8 32
			*/
			void* target_addr = NULL;
			size_t byte_count = 64; // Default count block length
			if (scanf_s("%p %zu", &target_addr, &byte_count) > 0) 
			{
				DumpMemoryHex(process_handle, target_addr, byte_count);
			}
		}
		else if (strcmp(cmd, "bt") == 0) 
		{
			PrintCallStackBacktrace(process_handle, thread_handle);
		}
	}
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		printf("Usage: win32_debugger_symbols.exe <target_exe>\n");
		return 1;
	}

	STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi; ZeroMemory(&si, sizeof(si)); ZeroMemory(&pi, sizeof(pi));
	// CRITICAL PATH FIX: Pass ONLY target.exe to child cmdLine, preventing double name evaluation bugs
	char cmdLine[MAX_PATH] = { 0 };
	sprintf_s(cmdLine, sizeof(cmdLine), "\"%s\"", argv[1]);
	BOOL success = CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi);
	if (!success)
	{
		printf("[-] Target load error: %lu\n", GetLastError()); return 1;
	}

	DEBUG_EVENT de;
	bool keeps_debugging = true;
	while (keeps_debugging && WaitForDebugEvent(&de, INFINITE))
	{
		DWORD continue_status = DBG_CONTINUE;
		switch (de.dwDebugEventCode)
		{
		case CREATE_PROCESS_DEBUG_EVENT:
			g_base_address = (DWORD64)de.u.CreateProcessInfo.lpBaseOfImage;
			SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_ALLOW_ABSOLUTE_SYMBOLS);
			if (SymInitialize(pi.hProcess, NULL, FALSE))
			{
				g_symbols_initialized = true;
				LARGE_INTEGER fileSize;
				DWORD image_size = 0;
				if (GetFileSizeEx(de.u.CreateProcessInfo.hFile, &fileSize))
				{
					image_size = (DWORD)fileSize.LowPart;
				}

				if (SymLoadModule64(pi.hProcess, de.u.CreateProcessInfo.hFile, NULL, NULL, g_base_address, image_size))
				{
					printf("[+] Symbol Engine initialized successfully.\n");
					char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
					PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
					pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO); pSymbol->MaxNameLen = MAX_SYM_NAME;
					if (SymFromName(pi.hProcess, "main", pSymbol))
					{
						AddBreakpoint(pi.hProcess, (void*)pSymbol->Address);
						printf("[] Pre-registered core entrance breakpoint at 0x%p.\n", (void*)pSymbol->Address);
					}
				}
			}
			break;

		case EXCEPTION_DEBUG_EVENT: {
			DWORD exception_code = de.u.Exception.ExceptionRecord.ExceptionCode;
			void* exception_address = de.u.Exception.ExceptionRecord.ExceptionAddress;
			if (exception_code == EXCEPTION_BREAKPOINT)
			{
				CONTEXT ctx;
				ctx.ContextFlags = CONTEXT_CONTROL;
				GetThreadContext(pi.hThread, &ctx);
#ifdef _WIN64
				void* actual_exception_ip = (void*)ctx.Rip;
#else
				void* actual_exception_ip = (void*)ctx.Eip;
#endif
				void* target_bp_addr = (void*)((BYTE*)actual_exception_ip - 1);
				int hit_index = -1;
				for (int i = 0; i < MAX_BREAKPOINTS; i++)
				{
					if (g_breakpoints[i].is_set && g_breakpoints[i].address == target_bp_addr)
					{
						hit_index = i; break;
					}
				}
				if (hit_index != -1)
				{
					WriteTargetMemory(pi.hProcess, g_breakpoints[hit_index].address, &g_breakpoints[hit_index].original_byte, 1);
					bool was_temporary = g_breakpoints[hit_index].is_temporary;
					void* saved_address = g_breakpoints[hit_index].address;
					BYTE saved_byte = g_breakpoints[hit_index].original_byte;
					if (was_temporary)
					{
						g_breakpoints[hit_index].address = NULL;
						g_breakpoints[hit_index].is_set = false;
						g_breakpoints[hit_index].is_temporary = false;
					}
#ifdef _WIN64
					ctx.Rip -= 1;
#else
					ctx.Eip -= 1;
#endif
					SetThreadContext(pi.hThread, &ctx);
					if (!was_temporary)
					{
						printf("\n[+] Breakpoint Hit at address: 0x%p\n", saved_address);
					}
					else
					{
						printf("\n[+] Step Complete.\n");
					}
					DebuggerConsolePrompt(pi.hProcess, pi.hThread);
					// Re-inject breakpoint logic if persistent execution continue is requested
					if (!was_temporary && !g_is_single_stepping)
					{
						SetSingleStepTrap(pi.hThread, true);
						g_breakpoints[hit_index].address = saved_address;
						g_breakpoints[hit_index].original_byte = saved_byte;
						g_breakpoints[hit_index].is_set = true;
						g_breakpoints[hit_index].is_temporary = true;
					}
				}
				else
				{
					printf("\n[*] Windows Loader Stopped. Initial Attach OK.\n");
					DebuggerConsolePrompt(pi.hProcess, pi.hThread);
				}
			}
			else if (exception_code == EXCEPTION_SINGLE_STEP)
			{
				for (int i = 0; i < MAX_BREAKPOINTS; i++)
				{
					if (g_breakpoints[i].is_set && g_breakpoints[i].is_temporary && g_breakpoints[i].address != NULL)
					{
						BYTE int3_opcode = 0xCC;
						WriteTargetMemory(pi.hProcess, g_breakpoints[i].address, &int3_opcode, 1);
						g_breakpoints[i].is_temporary = false;
					}
				}
				if (g_is_single_stepping)
				{
					DebuggerConsolePrompt(pi.hProcess, pi.hThread);
				}
				else
				{
					SetSingleStepTrap(pi.hThread, false);
				}
			}
			else
			{
				continue_status = DBG_EXCEPTION_NOT_HANDLED;
			}
			break;
		}
		case EXIT_PROCESS_DEBUG_EVENT:
			keeps_debugging = false;
			break;
		}
		ContinueDebugEvent(de.dwProcessId, de.dwThreadId, continue_status);
	}
	if (g_symbols_initialized)
		SymCleanup(pi.hProcess);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return 0;
}