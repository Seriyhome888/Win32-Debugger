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
	DWORD flags;
	DWORD register_id;
	bool found;
} FindVariableContext;

bool ReadTargetMemory(void* process_handle, void* address, void* buffer, size_t size)
{
	SIZE_T bytes_read;
	return ReadProcessMemory((HANDLE)process_handle, address, buffer, size, &bytes_read) && (bytes_read == size);
}

void DumpSymbolData(HANDLE process_handle, DWORD64 mod_base, DWORD type_id, DWORD64 absolute_address, DWORD flags, const char* sym_name) 
{
	DWORD sym_tag = 0;
	SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_SYMTAG, &sym_tag);

	// --- CASE 1: Arrays ---
	if (sym_tag == SymTagArrayType) 
	{
		DWORD count = 0;
		DWORD type_id_child = 0;
		DWORD64 element_size = 0; // Capture explicit size of an individual element

		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_COUNT, &count);
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_TYPEID, &type_id_child);
		SymGetTypeInfo(process_handle, mod_base, type_id_child, TI_GET_LENGTH, &element_size);

		// If the symbol engine doesn't explicitly return a count, fallback to 5 elements safely
		if (count == 0) count = 5;

		printf("    %s = [ ", sym_name);
		for (DWORD i = 0; i < count; i++) 
		{
			int element_val = 0;
			// Multiply index iteration explicitly against the individual child element byte size
			void* elem_addr = (BYTE*)absolute_address + (i * element_size);

			if (ReadTargetMemory(process_handle, elem_addr, &element_val, (size_t)element_size)) 
			{
				printf("%d ", element_val);
			}
			else 
			{
				break;
			}
		}

		printf("] (Array of %lu elements)\n", count);
		return;
	}

	// --- CASE 2: Structs and Unions (UDT) ---
	if (sym_tag == SymTagUDT) 
	{
		DWORD children_count = 0;
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_CHILDRENCOUNT, &children_count);

		printf("    %s = Struct/Union (Members: %lu) at 0x%I64X:\n    {\n", sym_name, children_count, absolute_address);
		if (children_count == 0) 
		{
			printf("        <empty>\n    }\n");
			return;
		}

		DWORD find_children_size = sizeof(TI_FINDCHILDREN_PARAMS) + (children_count * sizeof(ULONG));
		TI_FINDCHILDREN_PARAMS* pParams = (TI_FINDCHILDREN_PARAMS*)malloc(find_children_size);
		if (!pParams) return;

		ZeroMemory(pParams, find_children_size);
		pParams->Count = children_count;

		if (SymGetTypeInfo(process_handle, mod_base, type_id, TI_FINDCHILDREN, pParams)) 
		{
			for (DWORD i = 0; i < children_count; i++) 
			{
				ULONG child_id = pParams->ChildId[i];
				WCHAR* child_name_w = NULL;
				DWORD member_offset = 0;
				DWORD member_type_id = 0;
				DWORD64 member_size = 0;
				DWORD member_sym_tag = 0;

				SymGetTypeInfo(process_handle, mod_base, child_id, TI_GET_SYMNAME, &child_name_w);
				SymGetTypeInfo(process_handle, mod_base, child_id, TI_GET_OFFSET, &member_offset);
				SymGetTypeInfo(process_handle, mod_base, child_id, TI_GET_TYPEID, &member_type_id);
				SymGetTypeInfo(process_handle, mod_base, member_type_id, TI_GET_LENGTH, &member_size);
				SymGetTypeInfo(process_handle, mod_base, member_type_id, TI_GET_SYMTAG, &member_sym_tag);

				void* member_absolute_addr = (BYTE*)absolute_address + member_offset;

				if (child_name_w) 
				{
					wprintf(L"        .%ls [Offset: +%lu, Size: %I64u]: ", child_name_w, member_offset, member_size);

					if (member_sym_tag == SymTagBaseType) 
					{
						long long scalar_value = 0;
						if (ReadTargetMemory(process_handle, member_absolute_addr, &scalar_value, (size_t)member_size)) 
						{
							if (member_size == 1) printf("%d\n", (char)scalar_value);
							else if (member_size == 2) printf("%d\n", (short)scalar_value);
							else if (member_size == 4) printf("%d\n", (int)scalar_value);
							else printf("%I64d\n", scalar_value);
						}
						else printf("<failed read>\n");
					}
					else if (member_sym_tag == SymTagPointerType) 
					{
						DWORD64 target_pointer_value = 0;
						if (ReadTargetMemory(process_handle, member_absolute_addr, &target_pointer_value, (size_t)member_size)) 
						{
							target_pointer_value &= 0xFFFFFFFF;
							DWORD ptr_child_type_id = 0;
							DWORD64 ptr_child_size = 0;
							SymGetTypeInfo(process_handle, mod_base, member_type_id, TI_GET_TYPEID, &ptr_child_type_id);
							SymGetTypeInfo(process_handle, mod_base, ptr_child_type_id, TI_GET_LENGTH, &ptr_child_size);

							if (ptr_child_size == 1) 
							{
								char str_buffer[64] = { 0 };
								bool read_any = false;
								for (size_t k = 0; k < sizeof(str_buffer) - 1; k++) 
								{
									char b = 0;
									if (ReadTargetMemory(process_handle, (void*)((DWORD_PTR)target_pointer_value + k), &b, 1)) 
									{
										read_any = true;
										str_buffer[k] = b;
										if (b == '\0') break;
									}
									else break;
								}
								if (read_any) printf("0x%I64X -> \"%s\"\n", target_pointer_value, str_buffer);
								else printf("0x%I64X -> <unmapped>\n", target_pointer_value);
							}
							else 
							{
								printf("0x%I64X -> points to size %I64u\n", target_pointer_value, ptr_child_size);
							}
						}
						else printf("<failed read pointer>\n");
					}
					else printf("<complex child struct>\n");

					LocalFree(child_name_w);
				}
			}
		}
		printf("    }\n");
		free(pParams);
		return;
	}

	// --- CASE 3: Pointers and Scalars ---
	DWORD child_type_id = 0;
	DWORD64 child_size = 0;
	SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_TYPEID, &child_type_id);
	SymGetTypeInfo(process_handle, mod_base, child_type_id, TI_GET_LENGTH, &child_size);

	if (sym_tag == SymTagPointerType) 
	{
		DWORD64 target_pointer_value = 0;
		if (ReadTargetMemory(process_handle, (void*)absolute_address, &target_pointer_value, sizeof(void*))) 
		{
			target_pointer_value &= 0xFFFFFFFF;
			if (child_size == 1) 
			{ // char*
				char str_buffer[64] = { 0 };
				for (size_t k = 0; k < sizeof(str_buffer) - 1; k++) 
				{
					char b = 0;
					if (ReadTargetMemory(process_handle, (void*)((DWORD_PTR)target_pointer_value + k), &b, 1)) 
					{
						str_buffer[k] = b;
						if (b == '\0') break;
					}
					else break;
				}
				printf("    %s (char*) = 0x%I64X -> \"%s\"\n", sym_name, target_pointer_value, str_buffer);
			}
			else 
			{
				printf("    %s (pointer) = 0x%I64X\n", sym_name, target_pointer_value);
			}
		}
		return;
	}

	// Default: Scalar Value
	int scalar_value = 0;
	if (ReadTargetMemory(process_handle, (void*)absolute_address, &scalar_value, sizeof(scalar_value))) 
	{
		printf("    %s = %d\n", sym_name, scalar_value);
	}
}

typedef struct {
	HANDLE process_handle;
	CONTEXT* ctx;
	DWORD64 mod_base;
} DumpLocalsContext;

BOOL CALLBACK EnumAllLocalsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) {
	DumpLocalsContext* context = (DumpLocalsContext*)UserContext;

	// Ignore compiler-generated internal placeholders or functions
	if (pSymInfo->Flags & SYMFLAG_CLR_TOKEN) return TRUE;

	DWORD64 absolute_address = 0;

	if (pSymInfo->Flags & SYMFLAG_REGREL) {
		int ebp_base = (int)context->ctx->Ebp;
		int esp_base = (int)context->ctx->Esp;
		int signed_displacement = (int)(pSymInfo->Address & 0xFFFFFFFF);

		if (pSymInfo->Register == 23 || pSymInfo->Register == 7) {
			absolute_address = (DWORD64)(esp_base + signed_displacement);
		}
		else {
			absolute_address = (DWORD64)(ebp_base + signed_displacement);
		}
	}
	else if (pSymInfo->Flags & SYMFLAG_REGISTER) {
		printf("    %s = <Stored inside CPU register>\n", pSymInfo->Name);
		return TRUE;
	}
	else {
		absolute_address = pSymInfo->Address;
	}

	// Run our helper layout engine!
	DumpSymbolData(context->process_handle, pSymInfo->ModBase, pSymInfo->TypeIndex, absolute_address, pSymInfo->Flags, pSymInfo->Name);

	return TRUE;
}

void PrintLocalVariables(HANDLE process_handle, HANDLE thread_handle) {
	if (!g_symbols_initialized) return;

	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(thread_handle, &ctx)) return;

	IMAGEHLP_STACK_FRAME sf = { 0 };
	sf.InstructionOffset = ctx.Eip;
	sf.FrameOffset = ctx.Ebp;
	sf.StackOffset = ctx.Esp;

	SymSetContext(process_handle, &sf, NULL);

	printf("[*] Dumping all local variables inside active stack layer:\n");

	DumpLocalsContext context = { process_handle, &ctx, 0 };
	// Passing 0 as BaseOfDll instructs DbgHelp to dump the immediate frame local scope variables
	if (!SymEnumSymbols(process_handle, 0, NULL, EnumAllLocalsCallback, &context)) {
		printf("[-] Failed to query frame symbols. Error: %lu\n", GetLastError());
	}
}

// Callback function executed for each symbol found within the current local frame scope
BOOL CALLBACK EnumSymbolsCallback(PSYMBOL_INFO pSymInfo, ULONG SymbolSize, PVOID UserContext) 
{
	FindVariableContext* context = (FindVariableContext*)UserContext;
	if (strcmp(pSymInfo->Name, context->target_name) == 0) 
	{
		context->address = pSymInfo->Address;
		context->type_id = pSymInfo->TypeIndex;
		context->mod_base = pSymInfo->ModBase;
		context->flags = pSymInfo->Flags;
		context->register_id = pSymInfo->Register;
		context->found = true;
		return FALSE;
	}
	return TRUE;
}

bool WriteTargetMemory(void* process_handle, void* address, const void* buffer, size_t size) 
{
	SIZE_T bytes_written;
	return WriteProcessMemory((HANDLE)process_handle, address, buffer, size, &bytes_written) && (bytes_written == size);
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

bool ResolveVariableDetails(HANDLE process_handle, HANDLE thread_handle, const char* var_name, DWORD64* out_address, DWORD* out_type_id, DWORD64* out_mod_base) 
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(thread_handle, &ctx)) return false;

	IMAGEHLP_STACK_FRAME sf = { 0 };
	sf.InstructionOffset = ctx.Eip; // Provide current EIP program counter linear address
	sf.FrameOffset = ctx.Ebp;       // Core frame stack layout anchor base pointer
	sf.StackOffset = ctx.Esp;       // Stack pointer context

	// Inform DbgHelp engine of the explicit execution landscape scope
	SymSetContext(process_handle, &sf, NULL);

	FindVariableContext search_ctx = { var_name, 0, 0, 0, 0, 0, false };
	if (SymEnumSymbols(process_handle, 0, NULL, EnumSymbolsCallback, &search_ctx) && search_ctx.found) 
	{

		*out_type_id = search_ctx.type_id;
		*out_mod_base = search_ctx.mod_base;

		// 1. Process Register Relative Frame Storage (SYMFLAG_REGREL)
		if (search_ctx.flags & SYMFLAG_REGREL) 
		{
			// CRITICAL x86 FIX: Force both the register base value and the displacement
			// into signed 32-bit integers. This ensures a value like 0xFFFFFFBC wraps 
			// around natively to perform subtraction (EBP - offset) inside the 32-bit space.
			int ebp_base = (int)ctx.Ebp;
			int esp_base = (int)ctx.Esp;
			int signed_displacement = (int)(search_ctx.address & 0xFFFFFFFF);

			// Choose the parent alignment tracking register based on PDB specifications
			if (search_ctx.register_id == 23 || search_ctx.register_id == 7) 
			{ // ESP
				*out_address = (DWORD64)(esp_base + signed_displacement);
			}
			else 
			{ // Default to standard EBP layout tracking
				*out_address = (DWORD64)(ebp_base + signed_displacement);
			}
			return true;
		}

		// 2. Safeguard against variables optimized down into registers
		if (search_ctx.flags & SYMFLAG_REGISTER) 
		{
			printf("[-] Variable '%s' is stored purely inside a register and has no memory address.\n", var_name);
			return false;
		}

		// Fallback for global variables (flat static virtual addresses)
		*out_address = search_ctx.address;
		return true;
	}
	return false;
}

void PrintVariableByName(HANDLE process_handle, HANDLE thread_handle, const char* express) 
{
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

	if (!ResolveVariableDetails(process_handle, thread_handle, var_name, &absolute_address, &type_id, &mod_base)) 
	{
		printf("[-] Symbol variable '%s' could not be resolved in the current active local stack frame layer.\n", var_name);
		return;
	}

	DWORD sym_tag = 0;
	SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_SYMTAG, &sym_tag);

	if (sym_tag == SymTagArrayType) 
	{
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

	if (sym_tag == SymTagPointerType || dereference) 
	{
		DWORD64 target_pointer_value = 0;

		// 1. Read the actual address stored inside the pointer variable
		if (!ReadTargetMemory(process_handle, (void*)absolute_address, &target_pointer_value, sizeof(void*))) 
		{
			printf("[-] Failed to read pointer variable base value.\n");
			return;
		}

		// 2. Determine what type of data the pointer points to
		DWORD child_type_id = 0;
		DWORD64 child_size = 0;
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_TYPEID, &child_type_id);
		SymGetTypeInfo(process_handle, mod_base, child_type_id, TI_GET_LENGTH, &child_size);

		// 3. Handle character arrays (Strings)
		if (child_size == 1 && !dereference) 
		{
			char str_buffer[256] = { 0 };
			bool read_success = false;
			size_t bytes_gathered = 0;

			// Read byte-by-byte up to your expected buffer threshold or until a null-terminator
			for (size_t i = 0; i < sizeof(str_buffer) - 1; i++) 
			{
				char single_byte = 0;
				void* current_ptr = (BYTE*)target_pointer_value + i;

				if (ReadTargetMemory(process_handle, current_ptr, &single_byte, 1)) 
				{
					read_success = true; // We successfully read at least the start
					str_buffer[i] = single_byte;
					bytes_gathered++;
					if (single_byte == '\0') break; // Safe termination
				}
				else 
				{
					// Hit unmapped memory boundary or end of heap allocation block
					break;
				}
			}

			if (read_success && bytes_gathered > 0) 
			{
				printf("[+] Variable '%s' (char*) points to string: \"%s\" (at 0x%I64X)\n",
					var_name, str_buffer, target_pointer_value);
			}
			else 
			{
				DWORD err = GetLastError();
				printf("[-] Complete failure reading string at 0x%I64X. Win32 Error Code: %lu\n",
					target_pointer_value, err);
			}
			return;
		}

		// 4. Handle Dynamic Arrays vs Single Scalar Pointers
		DWORD elements_to_print = 4; // Default preview limit for actual arrays

		// Query the child symbol tag to see WHAT we are pointing to
		DWORD child_sym_tag = 0;
		SymGetTypeInfo(process_handle, mod_base, child_type_id, TI_GET_SYMTAG, &child_sym_tag);

		// CRITICAL FIX: If it points directly to a base scalar type, we only print 1 element!
		if (child_sym_tag == SymTagBaseType) 
		{
			elements_to_print = 1;
		}

		if (elements_to_print == 1) 
		{
			// Treat as a clean single scalar pointer dereference
			int scalar_val = 0;
			if (ReadTargetMemory(process_handle, (void*)target_pointer_value, &scalar_val, (size_t)child_size)) 
			{
				printf("[+] Variable '%s' (int*) points to value: %d (at 0x%I64X)\n", var_name, scalar_val, target_pointer_value);
			}
			else 
			{
				printf("[-] Failed to read pointer destination memory at 0x%I64X\n", target_pointer_value);
			}
		}
		else 
		{
			// Treat as a dynamic array layout
			printf("[+] Variable '%s' resolved as Dynamic Array at 0x%I64X:\n", var_name, target_pointer_value);
			printf("    Values: [ ");
			for (DWORD i = 0; i < elements_to_print; i++) 
			{
				void* elem_addr = (BYTE*)target_pointer_value + (i * child_size);
				int scalar_val = 0;
				if (ReadTargetMemory(process_handle, elem_addr, &scalar_val, (size_t)child_size)) 
				{
					printf("%d ", scalar_val);
				}
				else 
				{
					break;
				}
			}
			printf("... ]\n");
		}
		return;
	}

	//Parse structs/unions
	if (sym_tag == SymTagUDT) 
	{
		// 1. Get the number of children (members) inside the UDT
		DWORD children_count = 0;
		SymGetTypeInfo(process_handle, mod_base, type_id, TI_GET_CHILDRENCOUNT, &children_count);

		printf("[+] Variable '%s' resolved as Struct/Union (Members: %lu) at 0x%I64X:\n", var_name, children_count, absolute_address);

		if (children_count == 0) 
		{
			printf("    { <empty or opaque type> }\n");
			return;
		}

		// 2. Allocate buffer space to retrieve the child type indices
		DWORD find_children_size = sizeof(TI_FINDCHILDREN_PARAMS) + (children_count * sizeof(ULONG));
		TI_FINDCHILDREN_PARAMS* pParams = (TI_FINDCHILDREN_PARAMS*)malloc(find_children_size);
		if (!pParams) return;

		ZeroMemory(pParams, find_children_size);
		pParams->Count = children_count;
		pParams->Start = 0;

		// 3. Populate child indices into the allocation array
		if (SymGetTypeInfo(process_handle, mod_base, type_id, TI_FINDCHILDREN, pParams)) 
		{
			printf("    {\n");

			for (DWORD i = 0; i < children_count; i++) 
			{
				ULONG child_id = pParams->ChildId[i];

				// 4. Retrieve the member's Name string literal
				WCHAR* child_name_w = NULL;
				SymGetTypeInfo(process_handle, mod_base, child_id, TI_GET_SYMNAME, &child_name_w);

				// 5. Retrieve the member's memory byte offset relative to the struct start
				DWORD member_offset = 0;
				SymGetTypeInfo(process_handle, mod_base, child_id, TI_GET_OFFSET, &member_offset);

				// 6. Retrieve the underlying child data type profile
				DWORD member_type_id = 0;
				DWORD64 member_size = 0;
				SymGetTypeInfo(process_handle, mod_base, child_id, TI_GET_TYPEID, &member_type_id);
				SymGetTypeInfo(process_handle, mod_base, member_type_id, TI_GET_LENGTH, &member_size);

				// Fetch the explicit Type Tag of the member to handle it dynamically
				DWORD member_sym_tag = 0;
				SymGetTypeInfo(process_handle, mod_base, member_type_id, TI_GET_SYMTAG, &member_sym_tag);

				void* member_absolute_addr = (BYTE*)absolute_address + member_offset;

				if (child_name_w) 
				{
					wprintf(L"        .%ls [Offset: +%lu, Size: %I64u]: ", child_name_w, member_offset, member_size);

					// CASE A: Member is a simple scalar integer / primitive
					if (member_sym_tag == SymTagBaseType) 
					{
						long long scalar_value = 0; // Large enough container for 1, 2, 4, or 8 byte integers
						if (ReadTargetMemory(process_handle, member_absolute_addr, &scalar_value, (size_t)member_size)) 
						{
							// Mask according to actual size to avoid high-byte bleeding
							if (member_size == 1) printf("%d\n", (char)scalar_value);
							else if (member_size == 2) printf("%d\n", (short)scalar_value);
							else if (member_size == 4) printf("%d\n", (int)scalar_value);
							else printf("%I64d\n", scalar_value);
						}
						else 
						{
							printf("<failed to read base type>\n");
						}
					}

					// CASE B: Member is a pointer (like char* name)
					else if (member_sym_tag == SymTagPointerType) 
					{
						DWORD64 target_pointer_value = 0;
						// Read the full pointer size (4 bytes on x86, 8 bytes on x64)
						if (ReadTargetMemory(process_handle, member_absolute_addr, &target_pointer_value, (size_t)member_size)) 
						{

							// CRITICAL x86 BRIDGE: Mask out high-order garbage bits from 64-bit register allocation
							target_pointer_value &= 0xFFFFFFFF;

							// Query what type of variable the pointer is pointing to
							DWORD ptr_child_type_id = 0;
							DWORD64 ptr_child_size = 0;
							SymGetTypeInfo(process_handle, mod_base, member_type_id, TI_GET_TYPEID, &ptr_child_type_id);
							SymGetTypeInfo(process_handle, mod_base, ptr_child_type_id, TI_GET_LENGTH, &ptr_child_size);

							if (ptr_child_size == 1) 
							{ // It's a char* string pointer!
								char str_buffer[256] = { 0 }; // Explicitly size the buffer array footprint
								bool read_any = false;

								for (size_t k = 0; k < sizeof(str_buffer) - 1; k++) 
								{
									char b = 0;
									// Explicitly cast to clean 32-bit memory boundary pointer
									void* source_char_addr = (void*)((DWORD_PTR)target_pointer_value + k);

									if (ReadTargetMemory(process_handle, source_char_addr, &b, 1)) 
									{
										read_any = true;
										str_buffer[k] = b;
										if (b == '\0') break;
									}
									else 
									{
										break;
									}
								}

								if (read_any) printf("0x%I64X -> \"%s\"\n", target_pointer_value, str_buffer);
								else printf("0x%I64X -> <unmapped/empty string>\n", target_pointer_value);
							}
							else 
							{
								// Pointer to an integer or another structural component
								printf("0x%I64X -> pointing to type size %I64u\n", target_pointer_value, ptr_child_size);
							}
						}
						else 
						{
							printf("<failed to read pointer address>\n");
						}
					}

					// CASE C: Fallback structural element parsing
					else 
					{
						printf("<complex UDT nested structure>\n");
					}
				}

				// DbgHelp allocates the name with LocalAlloc, so we must clean it up
				if (child_name_w) 
				{
					LocalFree(child_name_w);
				}
			}
			printf("    }\n");
		}

		free(pParams);
		return;
	}

	int scalar_value = 0;
	if (ReadTargetMemory(process_handle, (void*)absolute_address, &scalar_value, sizeof(scalar_value))) 
	{
		printf("[+] Variable '%s' (at 0x%I64X) value: %d\n", var_name, absolute_address, scalar_value);
	}
}

void ModifyVariableByName(HANDLE process_handle, HANDLE thread_handle, const char* var_name, int new_value) 
{
	if (!g_symbols_initialized) return;

	DWORD64 absolute_address = 0;
	DWORD type_id = 0;
	DWORD64 mod_base = 0;

	if (ResolveVariableDetails(process_handle, thread_handle, var_name, &absolute_address, &type_id, &mod_base)) 
	{
		if (WriteTargetMemory(process_handle, (void*)absolute_address, &new_value, sizeof(new_value))) 
		{
			printf("[+] Successfully patched '%s' to value: %d\n", var_name, new_value);
		}
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
}

void HandleStepOver(HANDLE process_handle, HANDLE thread_handle) 
{
	CONTEXT ctx;
	ctx.ContextFlags = CONTEXT_CONTROL;
	GetThreadContext(thread_handle, &ctx);

	BYTE instruction_buffer[4] = { 0 };
#ifdef _WIN64
	void* current_ip = (void*)ctx.Rip;
#else
	void* current_ip = (void*)ctx.Eip;
#endif

	if (ReadTargetMemory(process_handle, current_ip, instruction_buffer, sizeof(instruction_buffer))) 
	{
		BYTE opcode = instruction_buffer[0];
		if (opcode == 0xE8) {
			void* next_instruction = (void*)((BYTE*)current_ip + 5);
			AddBreakpointExtended(process_handle, next_instruction, true);
			g_is_single_stepping = false;
			return;
		}
		else if (opcode == 0xFF && (instruction_buffer[1] & 0x30) == 0x10) 
		{
			void* next_instruction = (void*)((BYTE*)current_ip + 2); AddBreakpointExtended(process_handle, next_instruction, true); g_is_single_stepping = false; return;
		}
	}
	
	g_is_single_stepping = true; 
	SetSingleStepTrap(thread_handle, true);
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
		printf("\n[dbgr]> "); 
		if (scanf_s("%63s", cmd, (unsigned int)sizeof(cmd)) <= 0) continue;

		if (strcmp(cmd, "help") == 0)
		{
			printf("Commands:\n  r             - Print registers");
			printf("         \n  si            - Step Into");
			printf("		 \n  so            - Step Over");
			printf("         \n  out           - Step Out"); 
			printf("         \n  c             - Continue"); 
			printf("         \n  bt            - Backtrace");
			printf("         \n  x             - Hex dump");
			printf("         \n  print         - Evaluate variable / pointer(e.g., print my_ptr, print my_array)");
			printf("         \n  set           - Set value");
			printf("         \n  bp            - Set line breakpoint(e.g., bp target.c 27)");
			printf("         \n  wp            - Set Watchpoint");
			printf("		 \n  [info] locals - Print local scope varaibled info\n");
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
		else if (strcmp(cmd, "locals") == 0 || strcmp(cmd, "info locals") == 0) 
		{
			PrintLocalVariables(process_handle, thread_handle);
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




