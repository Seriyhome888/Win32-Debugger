// win32_debugger_symbols.c
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>

#pragma comment(lib, "dbghelp.lib")
#include <dbghelp.h>

#define MAX_BREAKPOINTS 16

// Symbol Tag definitions from CVCONST.H
enum SymTagEnum {
	SymTagNull, SymTagExe, SymTagCompiland, SymTagCompilandDetails, SymTagCompilandEnv,
	SymTagFunction, SymTagBlock, SymTagData, SymTagAnnotation, SymTagLabel,
	SymTagPublicSymbol, SymTagUDT, SymTagEnum, SymTagFunctionType, SymTagPointerType,
	SymTagArrayType, SymTagBaseType, SymTagTypedef, SymTagBaseClass, SymTagFriend,
	SymTagFunctionArgType, SymTagFuncDebugStart, SymTagFuncDebugEnd, SymTagUsingNamespace,
	SymTagVTableShape, SymTagVTable, SymTagCustom, SymTagThunk, SymTagCustomType,
	SymTagManagedType, SymTagDimension
};

typedef struct {
	void* address;
	BYTE original_byte;
	bool is_set;
	bool is_temporary;
} Breakpoint;

// Global debugger states
Breakpoint g_breakpoints[MAX_BREAKPOINTS] = { 0 };
bool g_is_single_stepping = false;
bool g_symbols_initialized = false;
DWORD64 g_base_address = 0;
void* g_watched_address = NULL;

typedef struct {
	const char* target_name;
	DWORD64 address;
	DWORD type_id;
	DWORD64 mod_base;
	bool found;
} FindVariableContext;

// Callback function executed for each symbol found within the current local frame scope
BOOL CALLBACK EnumSymbolsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) {
	FindVariableContext* context = (FindVariableContext*)UserContext;
	if (strcmp(pSymInfo->Name, context->target_name) == 0) {
		context->address = pSymInfo->Address;
		context->type_id = pSymInfo->TypeIndex;
		context->mod_base = pSymInfo->ModBase;
		context->found = true;
		return FALSE;
	}
	return TRUE;
}

bool ReadTargetMemory(void* process_handle, void* address, void* buffer, size_t size) {
	SIZE_T bytes_read;
	return ReadProcessMemory((HANDLE)process_handle, address, buffer, size, &bytes_read) && (bytes_read == size);
}

bool WriteTargetMemory(void* process_handle, void* address, const void* buffer, size_t size) {
	SIZE_T bytes_written;
	return WriteProcessMemory((HANDLE)process_handle, address, buffer, size, &bytes_written) && (bytes_written == size);
}

bool AddBreakpointExtended(HANDLE process_handle, void* address, bool is_temp) {
	for (int i = 0; i < MAX_BREAKPOINTS; i++) {
		if (g_breakpoints[i].is_set && g_breakpoints[i].address == address) return true;
	}
	for (int i = 0; i < MAX_BREAKPOINTS; i++) {
		if (!g_breakpoints[i].is_set) {
			BYTE original;
			if (ReadTargetMemory(process_handle, address, &original, 1)) {
				BYTE int3_opcode = 0xCC;
				if (WriteTargetMemory(process_handle, address, &int3_opcode, 1)) {
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

bool AddBreakpoint(HANDLE process_handle, void* address) {
	return AddBreakpointExtended(process_handle, address, false);
}

void SetSingleStepTrap(HANDLE thread_handle, bool enable) {
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL;
	GetThreadContext(thread_handle, &ctx);
	if (enable) ctx.EFlags |= 0x100;
	else ctx.EFlags &= ~0x100;
	SetThreadContext(thread_handle, &ctx);
}

bool ResolveVariableDetails(HANDLE process_handle, HANDLE thread_handle, const char* var_name, DWORD64* out_address, DWORD* out_type_id, DWORD64* out_mod_base) {
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

	FindVariableContext search_ctx = { var_name, 0, 0, 0, false };
	if (SymEnumSymbols(process_handle, 0, NULL, EnumSymbolsCallback, &search_ctx) && search_ctx.found) {
		int stack_offset = (int)(search_ctx.address & 0xFFFFFFFF);
		if (search_ctx.address >= 0x8000000000000000ULL || stack_offset < 0 || search_ctx.address < 0x10000) {
#ifdef _WIN64
			* out_address = ctx.Rbp + stack_offset;
#else
			* out_address = ctx.Ebp + stack_offset;
#endif
		}
		else {
			*out_address = search_ctx.address;
		}
		*out_type_id = search_ctx.type_id;
		*out_mod_base = search_ctx.mod_base;
		return true;
	}
	return false;
}

void PrintVariableByName(HANDLE process_handle, HANDLE thread_handle, const char* express) {
	if (!g_symbols_initialized) return;

	bool dereference = false;
	const char* var_name = express;

	if (*express == '*') {
		dereference = true;
		var_name = express + 1;
	}

	DWORD64 absolute_address = 0;
	DWORD type_id = 0;
	DWORD64 mod_base = 0;

	if (!ResolveVariableDetails(process_handle, thread_handle, var_name, &absolute_address, &type_id, &mod_base)) {
		printf("[-] Symbol variable '%s' could not be resolved in the current active local stack frame layer.\n", var_name);
		return;
	}

	DWORD sym_tag = 0;
	SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_SYMTAG, &sym_tag);

	if (sym_tag == SymTagArrayType) {
		DWORD count = 0;
		DWORD type_id_child = 0;
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_COUNT, &count);
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_TYPEID, &type_id_child);

		if (count == 0) count = 5;

		printf("[+] Variable '%s' resolved as Array (Elements: %lu) at Stack Pointer 0x%I64X:\n", var_name, count, absolute_address);
		printf("    Values: [ ");
		for (DWORD i = 0; i < count; i++) {
			int element_val = 0;
			void* elem_addr = (BYTE*)absolute_address + (i * sizeof(int));
			ReadTargetMemory(process_handle, elem_addr, &element_val, sizeof(element_val));
			printf("%d ", element_val);
		}
		printf("]\n");
		return;
	}

	if (sym_tag == SymTagPointerType || dereference) {
		DWORD64 target_pointer_value = 0;

		if (!ReadTargetMemory(process_handle, (void*)absolute_address, &target_pointer_value, sizeof(void*))) {
			printf("[-] Failed to read pointer variable base value.\n");
			return;
		}

		DWORD child_type_id = 0;
		DWORD64 child_size = 0;
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_TYPEID, &child_type_id);
		SymGetTypeInfo(process_handle, mod_base, child_type_id, TI_GET_LENGTH, &child_size);

		if (child_size == 1 && !dereference) {
			char str_buffer[256] = { 0 };
			ReadTargetMemory(process_handle, (void*)target_pointer_value, str_buffer, sizeof(str_buffer) - 1);
			printf("[+] Variable '%s' (char*) points to string literal: \"%s\" (at 0x%I64X)\n", var_name, str_buffer, target_pointer_value);
		}
		else {
			int pointed_value = 0;
			if (ReadTargetMemory(process_handle, (void*)target_pointer_value, &pointed_value, sizeof(pointed_value))) {
				printf("[+] Expression Dereference '*%s' (at 0x%I64X) target value: %d\n", var_name, target_pointer_value, pointed_value);
			}
			else {
				printf("[-] Failed to dereference pointer address location 0x%I64X\n", target_pointer_value);
			}
		}
		return;
	}

	int scalar_value = 0;
	if (ReadTargetMemory(process_handle, (void*)absolute_address, &scalar_value, sizeof(scalar_value))) {
		printf("[+] Variable '%s' (at 0x%I64X) value: %d\n", var_name, absolute_address, scalar_value);
	}
}

void ModifyVariableByName(HANDLE process_handle, HANDLE thread_handle, const char* var_name, int new_value) {
	if (!g_symbols_initialized) return;

	DWORD64 absolute_address = 0;
	DWORD type_id = 0;
	DWORD64 mod_base = 0;

	if (ResolveVariableDetails(process_handle, thread_handle, var_name, &absolute_address, &type_id, &mod_base)) {
		if (WriteTargetMemory(process_handle, (void*)absolute_address, &new_value, sizeof(new_value))) {
			printf("[+] Successfully patched '%s' to value: %d\n", var_name, new_value);
		}
	}
}

void SetLineBreakpoint(HANDLE process_handle, const char* filename, int line_number) {
	IMAGEHLP_LINE64 line_info = { 0 };
	line_info.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
	LONG displacement = 0;

	if (SymGetLineFromName64(process_handle, NULL, filename, line_number, &displacement, &line_info)) {
		void* bp_address = (void*)line_info.Address;
		if (AddBreakpoint(process_handle, bp_address)) {
			printf("[+] Successfully set line breakpoint at %s:%d (Address: 0x%p)\n", filename, line_number, bp_address);
		}
	}
}

void HandleStepOver(HANDLE process_handle, HANDLE thread_handle) {
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL;
	GetThreadContext(thread_handle, &ctx);

	BYTE instruction_buffer[4] = { 0 };
#ifdef _WIN64
	void* current_ip = (void*)ctx.Rip;
#else
	void* current_ip = (void*)ctx.Eip;
#endif

	if (ReadTargetMemory(process_handle, current_ip, instruction_buffer, sizeof(instruction_buffer))) {
		BYTE opcode = instruction_buffer[0];
		if (opcode == 0xE8) {
			void* next_instruction = (void*)((BYTE*)current_ip + 5);
			AddBreakpointExtended(process_handle, next_instruction, true);
			g_is_single_stepping = false;
			return;
		}
		else if (opcode == 0xFF && (instruction_buffer[1] & 0x30) == 0x10) {
			void* next_instruction = (void*)((BYTE*)current_ip + 2); AddBreakpointExtended(process_handle, next_instruction, true); g_is_single_stepping = false; return;
		}
	}g_is_single_stepping = true; SetSingleStepTrap(thread_handle, true);
}

void HandleStepOut(HANDLE process_handle, HANDLE thread_handle)
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER; GetThreadContext(thread_handle, &ctx);
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

		printf("0x%p:  ", current_row_addr);
		for (size_t i = 0; i < 16; i++)
		{
			if (i < bytes_to_read)
				printf("%02X ", buffer[i]); else printf("   ");
			if (i == 7) printf(" ");
		}

		printf(" | ");
		for (size_t i = 0; i < bytes_to_read; i++)
		{
			char ch = (char)buffer[i];
			if (ch >= 32 && ch <= 126)
				printf("%c", ch);
			else printf(".");
		}

		printf("\n");
	}

	printf("----------------------------------------\n");
}

void PrintCallStackBacktrace(HANDLE process_handle, HANDLE thread_handle)
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(thread_handle, &ctx))
		return;

	STACKFRAME64 frame = { 0 };
	DWORD machine_type = IMAGE_FILE_MACHINE_I386;

#ifdef _WIN64    
	machine_type = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#else
	machine_type = IMAGE_FILE_MACHINE_I386;
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
	}

	printf("-----------------------------------\n");
}

bool SetHardwareWatchpoint(HANDLE thread_handle, void* address)
{
	CONTEXT ctx; ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if (!GetThreadContext(thread_handle, &ctx)) return false;
#ifdef _WIN64
	ctx.Dr0 = (DWORD64)address;
#else
	ctx.Dr0 = (DWORD32)address;
#endif
	ctx.Dr7 |= 0x1;
	ctx.Dr7 |= 0x10000;
	ctx.Dr7 |= 0x300000;
	g_watched_address = address;
	return SetThreadContext(thread_handle, &ctx);
}

void ClearHardwareWatchpoint(HANDLE thread_handle)
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if (GetThreadContext(thread_handle, &ctx))
	{
		ctx.Dr0 = 0;
		ctx.Dr7 &= ~0x1;
		ctx.Dr7 &= ~0x10000;
		ctx.Dr7 &= ~0x300000;
		SetThreadContext(thread_handle, &ctx);
	}
	g_watched_address = NULL;
}

void DebuggerConsolePrompt(HANDLE process_handle, HANDLE thread_handle)
{
	char cmd[64];
	while (true)
	{
		printf("\n[dbgr]> "); if (scanf_s("%63s", cmd, (unsigned int)sizeof(cmd)) <= 0) continue;

		if (strcmp(cmd, "help") == 0)
		{
			printf("Commands:\n  r            - Print registers\n  si           - Step Into\n  so           - Step Over\n  out          - Step Out\n  c            - Continue\n  bt           - Backtrace\n  x   - Hex dump\n  print  - Evaluate variable/pointer (e.g., print my_ptr, print my_array)\n  set    - Set value\n  bp     - Set line breakpoint\n  wp        - Set Watchpoint\n");
		}
		else if (strcmp(cmd, "r") == 0)
		{
			CONTEXT ctx;
			ctx.ContextFlags = CONTEXT_CONTROL;
			GetThreadContext(thread_handle, &ctx);
#ifdef _WIN64
			printf("Registers: RIP=0x%p, RSP=0x%p, RBP=0x%p\n", (void)ctx.Rip, (void*)ctx.Rsp, (void*)ctx.Rbp);
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
			g_is_single_stepping = false; break;
		}
		else if (strcmp(cmd, "bt") == 0)
		{
			PrintCallStackBacktrace(process_handle, thread_handle);
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
			size_t byte_count = 64;
			if (scanf_s("%p %zu", &target_addr, &byte_count) > 0)
			{
				DumpMemoryHex(process_handle, target_addr, byte_count);
			}
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
			char expr_arg[64];
			if (scanf_s("%63s", expr_arg, (unsigned int)sizeof(expr_arg)) > 0)
			{
				PrintVariableByName(process_handle, thread_handle, expr_arg);
			}
		}
		else if (strcmp(cmd, "set") == 0)
		{
			char var_name[64];
			int new_val = 0;
			if (scanf_s("%63s %d", var_name, (unsigned int)sizeof(var_name), &new_val) > 0)
			{
				ModifyVariableByName(process_handle, thread_handle, var_name, new_val);
			}
		}
		else if (strcmp(cmd, "wp") == 0)
		{
			char var_name[64];
			if (scanf_s("%63s", var_name, (unsigned int)sizeof(var_name)) > 0)
			{
				DWORD64 absolute_address = 0;
				DWORD dummy1 = 0;
				DWORD64 dummy2 = 0;
				if (ResolveVariableDetails(process_handle, thread_handle, var_name, &absolute_address, &dummy1, &dummy2))
				{
					if (SetHardwareWatchpoint(thread_handle, (void*)absolute_address))
					{
						printf("[+] Successfully armed CPU Debug Register DR0 watchpoint at 0x%I64X\n", absolute_address);
					}
				}
			}
		}
	}
}

int main(int argc, char* argv[])
{
	if (argc < 2) return 1;

#ifdef _WIN32
	// Set the console output code page to UTF-8
	SetConsoleOutputCP(CP_UTF8); // CP_UTF8 is defined as 65001
#endif

	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	char cmdLine[MAX_PATH] = { 0 };
	sprintf_s(cmdLine, sizeof(cmdLine), "\"%s\"", argv[1]);
	BOOL success = CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi);
	if (!success) return 1;

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
					pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
					pSymbol->MaxNameLen = MAX_SYM_NAME;
					if (SymFromName(pi.hProcess, "main", pSymbol))
					{
						AddBreakpoint(pi.hProcess, (void*)pSymbol->Address);
						printf("[*] Pre-registered core entrance breakpoint at 0x%I64X.\n", pSymbol->Address);
					}
				}
			}
			break;

		case EXCEPTION_DEBUG_EVENT:
		{
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
						hit_index = i;
						break;
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
						printf("\n[+] Line Breakpoint Hit at address: 0x%p\n", saved_address);
					else
						printf("\n[+] Step Complete.\n");
					DebuggerConsolePrompt(pi.hProcess, pi.hThread);
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
				CONTEXT debug_ctx;
				debug_ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				GetThreadContext(pi.hThread, &debug_ctx);
				if (g_watched_address != NULL && (debug_ctx.Dr6 & 0x1))
				{
					printf("\n[🔥 HARDWARE WATCHPOINT TRIGGERED] Memory write modification captured!\n");
					debug_ctx.Dr6 &= ~0x1;
					SetThreadContext(pi.hThread, &debug_ctx);
					ClearHardwareWatchpoint(pi.hThread);
					DebuggerConsolePrompt(pi.hProcess, pi.hThread);
				}
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




//
//*
//*PDB Expression Parsing : Evaluates raw string pointers(char*), maps multi - element local memory arrays, and handles dereferences(print* my_ptr).
//* Hardware Watchpoints : Leverages CPU debug registers(DR0 / DR7) to capture memory writes natively.
//* Stack backtracing& Hex Dumps : Includes StackWalk64 call frame traces(bt) and 16 - byte aligned binary hex matrix representations(x).
//* Execution Controls : Manages hardware trace steps(si), step - overs(so), and parent function returns(out).
//*
//