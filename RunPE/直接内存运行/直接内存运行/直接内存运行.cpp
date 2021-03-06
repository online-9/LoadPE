// 直接内存运行.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <windows.h>
#include <stdio.h>
#include <strsafe.h>
#include <tlhelp32.h>
#include "detours.h"

#pragma comment(lib, "detours.lib")

#define RVATOVA( base, offset )(((DWORD)(base) + (DWORD)(offset))) 

#define LDRP_RELOCATION_FINAL 0x2 

typedef struct 
{
	WORD	Offset:12;
	WORD	Type:4;
} IMAGE_FIXUP_ENTRY, *PIMAGE_FIXUP_ENTRY;

typedef struct _PEB
{
	DWORD smth[2];
	PVOID ImageBaseAddress;
} PEB, *PPEB;


typedef struct _TEB 
{
	DWORD smth[11];
	PVOID ThreadLocalStoragePointer;
	PPEB Peb;
} TEB, *PTEB;


WCHAR MUI_FILE_PATH[MAX_PATH] = {0};

//
//处理命令行用
//
static WCHAR  CMDLINE_W[MAX_PATH] ;
static CHAR  CMDLINE_A[MAX_PATH] ;

static WCHAR ModuleName_W[MAX_PATH];
static CHAR ModuleName_A[MAX_PATH];

static PVOID g_ImageBase = NULL;
static PVOID hMuiLoad = NULL;

LPWSTR *g_w_Argcx = NULL ;
LPSTR *g_Argcx = NULL ;


PVOID g_ImageBase0 = (PVOID)0x400000;
PVOID g_ImageBase1 = (PVOID)0x1000000;
SIZE_T g_ImageSize = 0x100000;
BOOL bMemReservOk = FALSE;

//////////////////////////////////////////////////////////////////////////
//
//For Loader 使用
//
PVOID 
MmVirtualAlloc( 
			   __in PVOID Base,
			   __in SIZE_T dwSize
			   )
{

	//
	//保留内存判断
	//
	if( (Base == g_ImageBase0 || Base == g_ImageBase1 ) && dwSize <= g_ImageSize && bMemReservOk)
	{

		printf("*** MmVirtualAlloc Reserve\n");
		return Base;
	}

	//////////////////////////////////////////////////////////////////////////
	PVOID pResult = VirtualAlloc( Base, 
		dwSize, 
		MEM_COMMIT|MEM_RESERVE, // 保留空间 和提交
		PAGE_EXECUTE_READWRITE 
		);

	if( pResult == NULL )
		goto _EXIT0;


_EXIT0:
	return pResult;
}

VOID 
MmVirtualFree(
			  __in          LPVOID lpAddress,
			  __in          SIZE_T dwSize
			  )
{

	printf("MemFree 0x%x  0x%x\n", lpAddress, dwSize );
	if( (lpAddress == g_ImageBase0 || lpAddress == g_ImageBase1 ) && dwSize <= g_ImageSize && bMemReservOk)
	{
		printf("*** MmVirtualFree Reserve\n");
		return ;
	}
	//
	//先MEM_DECOMMIT
	//
	VirtualFree( lpAddress, dwSize, MEM_DECOMMIT);

	//
	//再释放,MEM_RELEASE第二参数必须为0，见MSDN
	//

	VirtualFree( lpAddress, 0, MEM_RELEASE);

}




void GetHeaders(PCHAR ibase, PIMAGE_FILE_HEADER *pfh, PIMAGE_OPTIONAL_HEADER *poh, PIMAGE_SECTION_HEADER *psh)
{
	PIMAGE_DOS_HEADER mzhead = (PIMAGE_DOS_HEADER)ibase;
	*pfh = (PIMAGE_FILE_HEADER)&ibase[mzhead->e_lfanew];
	*pfh = (PIMAGE_FILE_HEADER)((PBYTE)*pfh + sizeof(IMAGE_NT_SIGNATURE));
	*poh = (PIMAGE_OPTIONAL_HEADER)((PBYTE)*pfh + sizeof(IMAGE_FILE_HEADER));
	*psh = (PIMAGE_SECTION_HEADER)((PBYTE)*poh + sizeof(IMAGE_OPTIONAL_HEADER));
}



PPEB GetPEB( )
{
	TEB* teb;
	__asm
	{
		mov eax, dword ptr fs:[18h]
		mov teb, eax
	}

	return teb->Peb;
}



//int __cdecl myfprintf( 
//			FILE *stream,
//			const char *format ,...
//			)
//{
//	va_list args;
//	int n;
//	char *sprint_buf = (char*)malloc(0x1000);
//	va_start(args, format);
//	n = vsprintf(sprint_buf, format, args);
//	va_end(args);
//
//	printf(sprint_buf);
//	free(sprint_buf);
//
//	return 0;
//}



void _LdrpCallTlsInitializers( HANDLE hModule, PIMAGE_TLS_DIRECTORY pTlsDir, DWORD fdwReason )
{
	__try
	{
		if( pTlsDir->AddressOfCallBacks )
		{
			PVOID *pCallBacks = (PVOID*)pTlsDir->AddressOfCallBacks;
			while(*pCallBacks)
			{
				PIMAGE_TLS_CALLBACK pTlsCallBack = (PIMAGE_TLS_CALLBACK)*pCallBacks;
				pCallBacks++;
				pTlsCallBack(hModule, fdwReason, 0 );
			}
		}

	}__except(EXCEPTION_EXECUTE_HANDLER)
	{

	}
}


void LdrInitThreadTls(PIMAGE_TLS_DIRECTORY pTlsDir)
{
	PVOID *ThreadLocalStoragePointer = NULL;
	UCHAR *pData = NULL;
	ULONG TlsSize = 0;
	ULONG TlsInitDataSize = 0;

	if( NtCurrentTeb()->ThreadLocalStoragePointer != NULL )
		return;

	TlsInitDataSize = pTlsDir->EndAddressOfRawData - pTlsDir->StartAddressOfRawData;	
	TlsSize = (pTlsDir->EndAddressOfRawData - pTlsDir->StartAddressOfRawData) + pTlsDir->SizeOfZeroFill;


	if( pTlsDir->StartAddressOfRawData )
	{
		ThreadLocalStoragePointer = (PVOID*)malloc(TlsSize+ sizeof(PVOID));
		pData = (UCHAR*)ThreadLocalStoragePointer + sizeof(PVOID);
		memcpy( pData, (void *)pTlsDir->StartAddressOfRawData, TlsInitDataSize );
		memset( pData + TlsInitDataSize, 0, pTlsDir->SizeOfZeroFill );
		NtCurrentTeb()->ThreadLocalStoragePointer = ThreadLocalStoragePointer;
		*(PVOID*)ThreadLocalStoragePointer = ThreadLocalStoragePointer;
	}


}


PIMAGE_BASE_RELOCATION 
PeLdrProcessRelocationBlock( 
							IN ULONG_PTR VA, 
							IN ULONG SizeOfBlock, 
							IN PUSHORT NextOffset, 
							IN LONGLONG Diff 
							) 
{ 
	PUCHAR FixupVA; 
	USHORT Offset; 
	LONG Temp; 
	ULONG Temp32; 
	ULONGLONG Value64; 
	LONGLONG Temp64; 


	while (SizeOfBlock--) { 

		Offset = *NextOffset & (USHORT)0xfff; 
		FixupVA = (PUCHAR)(VA + Offset); 

		// 
		// Apply the fixups. 
		// 

		switch ((*NextOffset) >> 12) { 

			case IMAGE_REL_BASED_HIGHLOW : 
				// 
				// HighLow - (32-bits) relocate the high and low half 
				//      of an address. 
				// 
				*(LONG UNALIGNED *)FixupVA += (ULONG) Diff; 
				break; 

			case IMAGE_REL_BASED_HIGH : 
				// 
				// High - (16-bits) relocate the high half of an address. 
				// 
				Temp = *(PUSHORT)FixupVA & 16; 
				Temp += (ULONG) Diff; 
				*(PUSHORT)FixupVA = (USHORT)(Temp >> 16); 
				break; 

			case IMAGE_REL_BASED_HIGHADJ : 
				// 
				// Adjust high - (16-bits) relocate the high half of an 
				//      address and adjust for sign extension of low half. 
				// 

				// 
				// If the address has already been relocated then don't 
				// process it again now or information will be lost. 
				// 
				if (Offset & LDRP_RELOCATION_FINAL) { 
					++NextOffset; 
					--SizeOfBlock; 
					break; 
				} 

				Temp = *(PUSHORT)FixupVA & 16; 
				++NextOffset; 
				--SizeOfBlock; 
				Temp += (LONG)(*(PSHORT)NextOffset); 
				Temp += (ULONG) Diff; 
				Temp += 0x8000; 
				*(PUSHORT)FixupVA = (USHORT)(Temp >> 16); 

				break; 

			case IMAGE_REL_BASED_LOW : 
				// 
				// Low - (16-bit) relocate the low half of an address. 
				// 
				Temp = *(PSHORT)FixupVA; 
				Temp += (ULONG) Diff; 
				*(PUSHORT)FixupVA = (USHORT)Temp; 
				break; 

			case IMAGE_REL_BASED_IA64_IMM64: 

				// 
				// Align it to bundle address before fixing up the 
				// 64-bit immediate value of the movl instruction. 
				// 

				FixupVA = (PUCHAR)((ULONG_PTR)FixupVA & ~(15)); 
				Value64 = (ULONGLONG)0; 

				// 
				// Extract the lower 32 bits of IMM64 from bundle 
				// 


				EXT_IMM64(Value64, 
					(PULONG)FixupVA + EMARCH_ENC_I17_IMM7B_INST_WORD_X, 
					EMARCH_ENC_I17_IMM7B_SIZE_X, 
					EMARCH_ENC_I17_IMM7B_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM7B_VAL_POS_X); 
				EXT_IMM64(Value64, 
					(PULONG)FixupVA + EMARCH_ENC_I17_IMM9D_INST_WORD_X, 
					EMARCH_ENC_I17_IMM9D_SIZE_X, 
					EMARCH_ENC_I17_IMM9D_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM9D_VAL_POS_X); 
				EXT_IMM64(Value64, 
					(PULONG)FixupVA + EMARCH_ENC_I17_IMM5C_INST_WORD_X, 
					EMARCH_ENC_I17_IMM5C_SIZE_X, 
					EMARCH_ENC_I17_IMM5C_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM5C_VAL_POS_X); 
				EXT_IMM64(Value64, 
					(PULONG)FixupVA + EMARCH_ENC_I17_IC_INST_WORD_X, 
					EMARCH_ENC_I17_IC_SIZE_X, 
					EMARCH_ENC_I17_IC_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IC_VAL_POS_X); 
				EXT_IMM64(Value64, 
					(PULONG)FixupVA + EMARCH_ENC_I17_IMM41a_INST_WORD_X, 
					EMARCH_ENC_I17_IMM41a_SIZE_X, 
					EMARCH_ENC_I17_IMM41a_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM41a_VAL_POS_X); 

				EXT_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM41b_INST_WORD_X), 
					EMARCH_ENC_I17_IMM41b_SIZE_X, 
					EMARCH_ENC_I17_IMM41b_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM41b_VAL_POS_X); 
				EXT_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM41c_INST_WORD_X), 
					EMARCH_ENC_I17_IMM41c_SIZE_X, 
					EMARCH_ENC_I17_IMM41c_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM41c_VAL_POS_X); 
				EXT_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_SIGN_INST_WORD_X), 
					EMARCH_ENC_I17_SIGN_SIZE_X, 
					EMARCH_ENC_I17_SIGN_INST_WORD_POS_X, 
					EMARCH_ENC_I17_SIGN_VAL_POS_X); 
				// 
				// Update 64-bit address 
				// 

				Value64+=Diff; 

				// 
				// Insert IMM64 into bundle 
				// 

				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM7B_INST_WORD_X), 
					EMARCH_ENC_I17_IMM7B_SIZE_X, 
					EMARCH_ENC_I17_IMM7B_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM7B_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM9D_INST_WORD_X), 
					EMARCH_ENC_I17_IMM9D_SIZE_X, 
					EMARCH_ENC_I17_IMM9D_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM9D_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM5C_INST_WORD_X), 
					EMARCH_ENC_I17_IMM5C_SIZE_X, 
					EMARCH_ENC_I17_IMM5C_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM5C_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IC_INST_WORD_X), 
					EMARCH_ENC_I17_IC_SIZE_X, 
					EMARCH_ENC_I17_IC_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IC_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM41a_INST_WORD_X), 
					EMARCH_ENC_I17_IMM41a_SIZE_X, 
					EMARCH_ENC_I17_IMM41a_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM41a_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM41b_INST_WORD_X), 
					EMARCH_ENC_I17_IMM41b_SIZE_X, 
					EMARCH_ENC_I17_IMM41b_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM41b_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_IMM41c_INST_WORD_X), 
					EMARCH_ENC_I17_IMM41c_SIZE_X, 
					EMARCH_ENC_I17_IMM41c_INST_WORD_POS_X, 
					EMARCH_ENC_I17_IMM41c_VAL_POS_X); 
				INS_IMM64(Value64, 
					((PULONG)FixupVA + EMARCH_ENC_I17_SIGN_INST_WORD_X), 
					EMARCH_ENC_I17_SIGN_SIZE_X, 
					EMARCH_ENC_I17_SIGN_INST_WORD_POS_X, 
					EMARCH_ENC_I17_SIGN_VAL_POS_X); 
				break; 

			case IMAGE_REL_BASED_DIR64: 

				*(ULONGLONG UNALIGNED *)FixupVA += Diff; 

				break; 

			case IMAGE_REL_BASED_MIPS_JMPADDR : 
				// 
				// JumpAddress - (32-bits) relocate a MIPS jump address. 
				// 
				Temp = (*(PULONG)FixupVA & 0x3ffffff) & 2; 
				Temp += (ULONG) Diff; 
				*(PULONG)FixupVA = (*(PULONG)FixupVA & ~0x3ffffff) | 
					((Temp >> 2) & 0x3ffffff); 

				break; 

			case IMAGE_REL_BASED_ABSOLUTE : 
				// 
				// Absolute - no fixup required. 
				// 
				break; 



			default : 
				// 
				// Illegal - illegal relocation type. 
				// 

				return (PIMAGE_BASE_RELOCATION)NULL; 
		} 
		++NextOffset; 
	} 
	return (PIMAGE_BASE_RELOCATION)NextOffset; 
} 

BOOLEAN 
PeLdrRelocateImage( 
				   IN PIMAGE_BASE_RELOCATION NextBlock,
				   IN DWORD NewBase,
				   IN int offsetRlc,
				   IN ULONG Size
				   ) 
{ 
	LONGLONG 	Diff; 
	ULONG 		TotalCountBytes = 0; 
	ULONG_PTR 		VA; 
	ULONGLONG 	OldBase; 
	ULONG 		SizeOfBlock; 
	PUCHAR 		FixupVA; 
	USHORT 		Offset; 
	PUSHORT 	NextOffset = NULL; 
	PIMAGE_NT_HEADERS NtHeaders; 
	BOOLEAN		Status; 


	TotalCountBytes = Size;

	Diff = (ULONG_PTR)offsetRlc; 
	while (TotalCountBytes)  
	{ 
		SizeOfBlock = NextBlock->SizeOfBlock; 
		TotalCountBytes -= SizeOfBlock; 
		SizeOfBlock -= sizeof(IMAGE_BASE_RELOCATION); 
		SizeOfBlock /= sizeof(USHORT); 
		NextOffset = (PUSHORT)((PCHAR)NextBlock + sizeof(IMAGE_BASE_RELOCATION)); 

		VA = (ULONG_PTR)NewBase + NextBlock->VirtualAddress; 
		if ( !(NextBlock = PeLdrProcessRelocationBlock( VA,SizeOfBlock,NextOffset,Diff )) )  
		{ 
			Status = FALSE; 
			goto Exit; 
		} 
	} 
	Status = TRUE;     
Exit:     
	return Status; 
} 


BOOL RelocPeModule(PIMAGE_BASE_RELOCATION pBlc, DWORD imagebase, int offsetRlc)
{
	DWORD vaddr, count, offset, type;
	WORD *items = NULL;

	while (NULL != pBlc->VirtualAddress) {
		vaddr = imagebase + pBlc->VirtualAddress;

		count = (pBlc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) >> 1;
		items = (WORD *)((char *)pBlc + sizeof(IMAGE_BASE_RELOCATION));

		for (DWORD i = 0; i < count; ++i) {
			offset = items[i] & 0x0fff;
			type = items[i] >> 12;

			if (type == 3) 
			{
				DWORD *Patch = (DWORD*)(vaddr + offset);


				*(DWORD *)(vaddr + offset) += offsetRlc;
			}
		}
		pBlc = (PIMAGE_BASE_RELOCATION)(items + count);
	}

	return TRUE;
}




BOOL FixupImport(PIMAGE_IMPORT_DESCRIPTOR pImp, DWORD imagebase)
{
	PIMAGE_THUNK_DATA pOrgThunk, pFirstThunk;
	PIMAGE_IMPORT_BY_NAME pImportName;

	while (NULL != pImp->OriginalFirstThunk)
	{
		pImp->Name += imagebase;

		FARPROC fpFun;
		HINSTANCE hInstance = LoadLibraryA((LPCSTR)pImp->Name);
		if (NULL == hInstance) 
		{
			printf("Load library %s failed, error: %x\n", pImp->Name, GetLastError());
			return FALSE;
		}

		printf("***** Dll %s\n", pImp->Name);

		//////////////////////////////////////////////////////////////////////////

		//////////////////////////////////////////////////////////////////////////

		pOrgThunk = (PIMAGE_THUNK_DATA)(imagebase + pImp->OriginalFirstThunk);
		pFirstThunk = (PIMAGE_THUNK_DATA)(imagebase + pImp->FirstThunk);

		while (NULL != *(DWORD *)pOrgThunk) 
		{
			if (pOrgThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) 
			{
				fpFun = GetProcAddress(hInstance, (LPCSTR)(pOrgThunk->u1.Ordinal & 0x0000ffff));

				if( fpFun == NULL )
				{
					//fpFun = (FARPROC)pFirstThunk->u1.Function;
					//printf("-  %x\n",pFirstThunk->u1.Function );
					////pImportName = (PIMAGE_IMPORT_BY_NAME)(imagebase + pFirstThunk->u1.AddressOfData);
					////fpFun = GetProcAddress(hInstance, (LPCSTR)pImportName->Name);
					//if( fpFun == NULL )
					printf("**** pOrgThunk->u1.Ordinal = %x Failed  %d\n", pOrgThunk->u1.Ordinal, GetLastError() );
					//__asm int 3
				}

			}else
			{
				pImportName = (PIMAGE_IMPORT_BY_NAME)(imagebase + pOrgThunk->u1.AddressOfData);
				fpFun = GetProcAddress(hInstance, (LPCSTR)pImportName->Name);


				if( fpFun == NULL )
				{
					printf("**** pImportName = %s Failed\n", pImportName );
					__asm int 3
				}
			}

			pFirstThunk->u1.Ordinal = (LONG)fpFun;

			++pFirstThunk;
			++pOrgThunk;
		}

		++pImp;
	}

	return TRUE;
}

BOOL FixupResource(PIMAGE_RESOURCE_DIRECTORY pRes, DWORD imagebase, int offsetRlc)
{
	PIMAGE_RESOURCE_DIRECTORY_ENTRY pEntry;
	DWORD nEntries;

	nEntries = pRes->NumberOfIdEntries + pRes->NumberOfNamedEntries;

	pEntry = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)((DWORD)pRes + sizeof(IMAGE_RESOURCE_DIRECTORY));

	for (DWORD i = 0; i < nEntries; ++i, ++pEntry) {

		if (IMAGE_RESOURCE_DATA_IS_DIRECTORY & pEntry->OffsetToData) {
			PIMAGE_RESOURCE_DIRECTORY pRes2;
			PIMAGE_RESOURCE_DIRECTORY_ENTRY pEntry2;
			DWORD nEntries2;

			pRes2 = (PIMAGE_RESOURCE_DIRECTORY)((DWORD)pRes
				+ (~IMAGE_RESOURCE_DATA_IS_DIRECTORY & pEntry->OffsetToData));
			nEntries2 = pRes2->NumberOfIdEntries + pRes2->NumberOfNamedEntries;
			pEntry2 = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)((DWORD)pRes2 + sizeof(IMAGE_RESOURCE_DIRECTORY));

			for (DWORD j = 0; j < nEntries2; ++j, ++pEntry2) {
				if (IMAGE_RESOURCE_NAME_IS_STRING & pEntry2->Name) {
					PIMAGE_RESOURCE_DIR_STRING_U pDirStr;
					pDirStr = (PIMAGE_RESOURCE_DIR_STRING_U)((DWORD)pRes
						+ (~IMAGE_RESOURCE_NAME_IS_STRING & pEntry2->Name));
				}
				if (IMAGE_RESOURCE_DATA_IS_DIRECTORY & pEntry2->OffsetToData) {
					PIMAGE_RESOURCE_DIRECTORY pRes3;
					PIMAGE_RESOURCE_DIRECTORY_ENTRY pEntry3;
					DWORD nEntries3;

					pRes3 = (PIMAGE_RESOURCE_DIRECTORY)((DWORD)pRes
						+ (~IMAGE_RESOURCE_DATA_IS_DIRECTORY & pEntry2->OffsetToData));
					nEntries3 = pRes3->NumberOfIdEntries + pRes3->NumberOfNamedEntries;
					pEntry3 = (PIMAGE_RESOURCE_DIRECTORY_ENTRY)((DWORD)pRes3 + sizeof(IMAGE_RESOURCE_DIRECTORY));

					for (DWORD k = 0; k < nEntries3; ++k) {
						PIMAGE_RESOURCE_DATA_ENTRY pData;

						pData = (PIMAGE_RESOURCE_DATA_ENTRY)((DWORD)pRes + pEntry3->OffsetToData);
						pData->OffsetToData += (DWORD)offsetRlc;
					}
				}
			}

		}
	}

	return TRUE;
}


//////////////////////////////////////////////////////////////////////////

BOOLEAN
LdrLoadSections(
	IN HMODULE MapBase,
	IN HANDLE hFile
	)
{
	BOOLEAN bResult = FALSE;
	if( MapBase == NULL ||  hFile == NULL )
		goto _EXIT0;

	PIMAGE_FILE_HEADER  FileHeader = NULL;
	PIMAGE_SECTION_HEADER  SectionHeader = NULL;
	PIMAGE_OPTIONAL_HEADER  OptionalHeader = NULL;
	PIMAGE_DOS_HEADER  DosHeader = NULL; 
	DWORD lpNumberOfBytesRead = 0;
	
	DosHeader = (PIMAGE_DOS_HEADER)MapBase;
	FileHeader = (PIMAGE_FILE_HEADER)( (DWORD)MapBase + DosHeader->e_lfanew + 4 );
	OptionalHeader = (PIMAGE_OPTIONAL_HEADER)( FileHeader + 1 );
	SectionHeader = (PIMAGE_SECTION_HEADER)( (DWORD)OptionalHeader + sizeof(IMAGE_OPTIONAL_HEADER) );

	for( int i=0; i < FileHeader->NumberOfSections ; i++, SectionHeader++ )
	{

		LPVOID VirtualAddress = (LPVOID)( (DWORD)MapBase + SectionHeader->VirtualAddress );
		OVERLAPPED lp = {0};

		lp.Offset = SectionHeader->PointerToRawData;

		if( ReadFile( hFile, VirtualAddress, SectionHeader->SizeOfRawData, &lpNumberOfBytesRead, &lp ) == FALSE || 
			lpNumberOfBytesRead != SectionHeader->SizeOfRawData 
			)
		{
			goto _EXIT0;
		}
	}

	bResult = TRUE;


_EXIT0:
	return bResult;
}

BOOLEAN
LdrFixupSection(
	IN DWORD MapBase
	)
{
	BOOLEAN bResult = FALSE;

	if( MapBase == FALSE )
		goto _EXIT0;

	PIMAGE_FILE_HEADER  FileHeader = NULL;
	PIMAGE_SECTION_HEADER  SectionHeader = NULL;
	PIMAGE_OPTIONAL_HEADER  OptionalHeader = NULL;
	PIMAGE_DOS_HEADER  DosHeader = NULL; 
	DWORD lpNumberOfBytesRead = 0;

	DosHeader = (PIMAGE_DOS_HEADER)MapBase;
	FileHeader = (PIMAGE_FILE_HEADER)( (DWORD)MapBase + DosHeader->e_lfanew + 4 );
	OptionalHeader = (PIMAGE_OPTIONAL_HEADER)( FileHeader + 1 );
	SectionHeader = (PIMAGE_SECTION_HEADER)( (DWORD)OptionalHeader + sizeof(IMAGE_OPTIONAL_HEADER) );

	for(int i = 0; i < FileHeader->NumberOfSections ; i++, SectionHeader++ )
	{
		DWORD VirtualSize = (i == FileHeader->NumberOfSections -1 )?
			( OptionalHeader->SizeOfImage - SectionHeader->VirtualAddress)
			:(SectionHeader + 1)->VirtualAddress - SectionHeader->VirtualAddress;
		LPVOID VirtualAddress = (LPVOID)( MapBase + SectionHeader->VirtualAddress );


		DWORD Attributes = PAGE_READWRITE;
		if( SectionHeader->Characteristics & IMAGE_SCN_MEM_EXECUTE || SectionHeader->Characteristics & IMAGE_SCN_MEM_READ )
		{
			if( SectionHeader->Characteristics & IMAGE_SCN_MEM_WRITE )
				Attributes = PAGE_READWRITE;
			else
				Attributes = PAGE_READONLY;
		}
		else if( SectionHeader->Characteristics & IMAGE_SCN_MEM_WRITE )
			Attributes = PAGE_READWRITE;


		if( !VirtualProtect( VirtualAddress, VirtualSize, Attributes, &Attributes ) )
		{
			printf("Cannot protect section [%d]\n", GetLastError());
			goto _EXIT0;
		}
	}

	bResult = TRUE;

_EXIT0:
	return bResult;
}



HMODULE
LoadExecutable (
  IN LPSTR path
  )

{
	HANDLE hFile = NULL;
	PIMAGE_FILE_HEADER		pfh;
	PIMAGE_SECTION_HEADER	psh;
	PIMAGE_OPTIONAL_HEADER poh;
	char buffer[ 0x1000 ];
	DWORD read;
	LPBYTE Mapping;
	BOOLEAN bInitResult = TRUE;
	PVOID ImageBase  = NULL;

	hFile = CreateFile( path, 
		GENERIC_READ, 
		FILE_SHARE_READ, 
		0, 
		OPEN_EXISTING, 
		0, 
		0 
		);
	if( hFile == INVALID_HANDLE_VALUE )
		return NULL;

	//
	//读取PE头
	//
	if( !ReadFile( hFile, 
		buffer, 
		1024, 
		&read, 
		0 
		) || !read 
		)
	{
		bInitResult = FALSE;
		goto _EXIT0;
	}

	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)buffer;
	pfh = (PIMAGE_FILE_HEADER)( (DWORD)buffer + dos->e_lfanew + 4 );
	poh = (PIMAGE_OPTIONAL_HEADER)( pfh+1 );
	psh = (PIMAGE_SECTION_HEADER)( (DWORD)poh + sizeof(IMAGE_OPTIONAL_HEADER) );

	ImageBase = (PVOID) poh->ImageBase;
	
	printf("Allocate [ Base = %08x, Size = %08x ]\n", ImageBase, poh->SizeOfImage);
	
	//
	//先把这片空间释放掉
	//
	//MmVirtualFree(ImageBase, poh->SizeOfImage);

	//__asm int 3
	//
	//从ImageBase开始分配大小为poh->SizeOfImage的内存
	//
	
	Mapping = (LPBYTE)MmVirtualAlloc( ImageBase, poh->SizeOfImage );
	if( Mapping == NULL )	
	{
		printf("\n\n*****mapped at 0x%x failed! %d******\n", ImageBase, GetLastError() );
		printf("可能导致失败\n", ImageBase, GetLastError() );
		Mapping = (LPBYTE)MmVirtualAlloc( 0, poh->SizeOfImage );
		if( Mapping == NULL )
		{
			bInitResult = FALSE;
			goto _EXIT0;
		}

	}

	//__asm INT 3
	printf("Old ImageBase  = 0x%08x  Mapped at 0x%08x\n", ImageBase, Mapping );

	g_ImageBase = Mapping;
	memcpy( Mapping, buffer, read );

	
	if( LdrLoadSections((HMODULE)Mapping, hFile) == FALSE )
	{
		bInitResult = FALSE;
		goto _EXIT0;
	}

	
	if( poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size )
	{	
		DWORD ImageBaseDelta = (DWORD)Mapping - (DWORD)poh->ImageBase;
		PIMAGE_BASE_RELOCATION Reloc = (PIMAGE_BASE_RELOCATION) RVATOVA(poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress, Mapping);
		

		//RelocPeModule(Reloc, (DWORD)Mapping, (DWORD)Mapping - (DWORD)poh->ImageBase );
		PeLdrRelocateImage( Reloc, (DWORD)Mapping, (DWORD)Mapping - (DWORD)poh->ImageBase,
			poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);

		
	}

	if( poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].Size )
	{
		PIMAGE_RESOURCE_DIRECTORY pRes = (PIMAGE_RESOURCE_DIRECTORY) RVATOVA(poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress, Mapping);
		FixupResource(pRes, (DWORD)Mapping, (DWORD)Mapping - poh->ImageBase);
	}
	
	if( poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size )
	{
		PIMAGE_IMPORT_DESCRIPTOR impdesc = (PIMAGE_IMPORT_DESCRIPTOR) RVATOVA(poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress, Mapping);
		FixupImport(impdesc, (DWORD)Mapping);
	}


	if( LdrFixupSection((DWORD)Mapping) == FALSE )
	{
		bInitResult = FALSE;
		goto _EXIT0;
	}

_EXIT0:
	if( bInitResult == FALSE )
	{
		MmVirtualFree(ImageBase, poh->SizeOfImage);
		Mapping = NULL;
	}

	if(hFile)
	CloseHandle( hFile );


	return (HMODULE) Mapping;
}


void
CallLoadedImage (
  IN HMODULE hModule
  )

{
	PIMAGE_FILE_HEADER		pfh;
	PIMAGE_OPTIONAL_HEADER	poh;
	PIMAGE_SECTION_HEADER	psh;
	GetHeaders((PCHAR) hModule, &pfh, &poh, &psh);

	DWORD lpflOldProtect = 0;
	VirtualProtect((void*)hModule, poh->SizeOfImage, PAGE_EXECUTE_READWRITE, &lpflOldProtect);

	PPEB peb = GetPEB( );
	peb->ImageBaseAddress = (PVOID) hModule;

	LPVOID entry = (LPVOID)( (DWORD)hModule + poh->AddressOfEntryPoint );

	printf("Run Now...\n");
	
	//__asm int 3
	__asm call dword ptr [entry];

}

void UnloadExecutable( HMODULE hModule )
{
	PIMAGE_FILE_HEADER		pfh;
	PIMAGE_OPTIONAL_HEADER	poh;
	PIMAGE_SECTION_HEADER	psh;
	GetHeaders((PCHAR) hModule, &pfh, &poh, &psh);

	//VirtualFree( hModule, poh->SizeOfImage, MEM_DECOMMIT );
	//VirtualFree( hModule, 0, MEM_RELEASE );
	
	MmVirtualFree((LPVOID)hModule, poh->SizeOfImage);


}



typedef DWORD ( WINAPI *pfnGetModuleFileNameA)(
	__in          HMODULE hModule,
	__out         LPSTR lpFilename,
	__in          DWORD nSize
	);

typedef DWORD ( WINAPI *pfnGetModuleFileNameW)(
	__in          HMODULE hModule,
	__out         LPWSTR lpFilename,
	__in          DWORD nSize
	);


typedef LPWSTR (WINAPI *pfnGetCommandLineW)(void);

typedef LPSTR (WINAPI *pfnGetCommandLineA)(void);

typedef VOID (WINAPI *pfnExitProcess)(__in   UINT uExitCode);

typedef DWORD ( WINAPI *pfnFormatMessageA)(
	__in          DWORD dwFlags,
	__in          LPCVOID lpSource,
	__in          DWORD dwMessageId,
	__in          DWORD dwLanguageId,
	__out         LPSTR lpBuffer,
	__in          DWORD nSize,
	__in          va_list* Arguments
	);

typedef DWORD ( WINAPI *pfnFormatMessageW)(
	__in          DWORD dwFlags,
	__in          LPCVOID lpSource,
	__in          DWORD dwMessageId,
	__in          DWORD dwLanguageId,
	__out         LPWSTR lpBuffer,
	__in          DWORD nSize,
	__in          va_list* Arguments
	);

typedef void (* pfnexit)( int status  );

typedef PVOID _startupinfo;

typedef int (*pfn__getmainargs)(
								int * _Argc, 
								char *** _Argv, 
								char *** _Env, 
								int _DoWildCard,
								_startupinfo * _StartInfo);

typedef int (*pfn__wgetmainargs)(
								 int *_Argc,
								 wchar_t ***_Argv,
								 wchar_t ***_Env,
								 int _DoWildCard,
								 _startupinfo * _StartInfo);

pfnFormatMessageA OldFormatMessageA = NULL;
pfnFormatMessageW OldFormatMessageW = NULL;
pfnGetModuleFileNameA OldGetModuleFileNameA = NULL;
pfnGetModuleFileNameW OldGetModuleFileNameW = NULL;
pfnGetCommandLineW OldGetCommandLineW = NULL;
pfnGetCommandLineA OldGetCommandLineA = NULL;
pfnExitProcess OldExitProcess = NULL;
pfnexit Oldexit = NULL;
pfn__getmainargs Old__getmainargs = NULL;
pfn__wgetmainargs Old__wgetmainargs = NULL;



PCHAR*
CommandLineToArgvA(
				   PCHAR CmdLine,
				   int* _argc
				   )
{
	PCHAR* argv;
	PCHAR  _argv;
	ULONG   len;
	ULONG   argc;
	CHAR   a;
	ULONG   i, j;

	BOOLEAN  in_QM;
	BOOLEAN  in_TEXT;
	BOOLEAN  in_SPACE;

	len = strlen(CmdLine);
	i = ((len+2)/2)*sizeof(PVOID) + sizeof(PVOID);

	argv = (PCHAR*)GlobalAlloc(GMEM_FIXED,
		i + (len+2)*sizeof(CHAR));

	_argv = (PCHAR)(((PUCHAR)argv)+i);

	argc = 0;
	argv[argc] = _argv;
	in_QM = FALSE;
	in_TEXT = FALSE;
	in_SPACE = TRUE;
	i = 0;
	j = 0;

	while( a = CmdLine[i] ) {
		if(in_QM) {
			if(a == '\"') {
				in_QM = FALSE;
			} else {
				_argv[j] = a;
				j++;
			}
		} else {
			switch(a) {
			  case '\"':
				  in_QM = TRUE;
				  in_TEXT = TRUE;
				  if(in_SPACE) {
					  argv[argc] = _argv+j;
					  argc++;
				  }
				  in_SPACE = FALSE;
				  break;
			  case ' ':
			  case '\t':
			  case '\n':
			  case '\r':
				  if(in_TEXT) {
					  _argv[j] = '\0';
					  j++;
				  }
				  in_TEXT = FALSE;
				  in_SPACE = TRUE;
				  break;
			  default:
				  in_TEXT = TRUE;
				  if(in_SPACE) {
					  argv[argc] = _argv+j;
					  argc++;
				  }
				  _argv[j] = a;
				  j++;
				  in_SPACE = FALSE;
				  break;
			}
		}
		i++;
	}
	_argv[j] = '\0';
	argv[argc] = NULL;

	(*_argc) = argc;
	return argv;
}



int Fake__getmainargs(
				  int * _Argc, 
				  char *** _Argv, 
				  char *** _Env, 
				  int _DoWildCard,
				  _startupinfo * _StartInfo)
{

	int Result = 0;
	
	Result = Old__getmainargs( _Argc, _Argv,_Env, _DoWildCard, _StartInfo );

	int Argc = 0;

	g_Argcx = CommandLineToArgvA(CMDLINE_A, &Argc);



	* _Argv = g_Argcx;

	* _Argc = Argc;

   
	return  Result;

}

int Fake__wgetmainargs (
					int *_Argc,
					wchar_t ***_Argv,
					wchar_t ***_Env,
					int _DoWildCard,
					_startupinfo * _StartInfo)
{

	//return 0;
	int Result = 0;

	Result = Old__wgetmainargs( _Argc, _Argv,_Env, _DoWildCard, _StartInfo );


	int Argc = 0;
	//g_w_Argcx = CommandLineToArgvW(CMDLINE_W, &Argc);
//////////////////////////////////////////////////////////////////////////
	//HMODULE hShell32 = LoadLibraryExA("shell32.dll", NULL, DONT_RESOLVE_DLL_REFERENCES);
	HMODULE hShell32 = LoadLibraryA("shell32.dll");
	if( hShell32 )
	{
		typedef LPWSTR* (WINAPI *__pfnCommandLineToArgvW)( LPCWSTR, int * ) ;
		__pfnCommandLineToArgvW pfnCommandLineToArgvW = NULL;

		pfnCommandLineToArgvW = (__pfnCommandLineToArgvW)GetProcAddress(hShell32, "CommandLineToArgvW");

		g_w_Argcx = pfnCommandLineToArgvW(CMDLINE_W, &Argc);
		printf("******0x%x\n", pfnCommandLineToArgvW);
		FreeLibrary(hShell32);
	}

	//////////////////////////////////////////////////////////////////////////
	* _Argv = g_w_Argcx;

	* _Argc = Argc;

	//for( int i = 0; i < Argc; i++ )
	//printf("---- %ws|\n", g_w_Argcx[i]);
	//
	
	return  Result;

}



void Fakeexit( 
		  int status 
		  )
{

	printf("Fakeexit = 0x%x\n", status );
	if( status == 0xbeebee )
		Oldexit(status);

	ExitThread(0xbeebee);

}


VOID WINAPI FakeExitProcess(
						__in          UINT uExitCode
						)
{

	if( uExitCode == 0xbeebee )
		OldExitProcess(0);

	ExitThread(0xbeebee);

}


LPWSTR 
WINAPI 
FakeGetCommandLineW(void)
{
	return CMDLINE_W;
}

LPSTR 
WINAPI 
FakeGetCommandLineA(void)
{
	return CMDLINE_A;
}



DWORD 
WINAPI 
FakeGetModuleFileNameW(
					   __in          HMODULE hModule,
					   __out         LPWSTR lpFilename,
					   __in          DWORD nSize
					   )
{


	DWORD dwRet = 0;
	//__asm int 3
	if( hModule == (HMODULE)g_ImageBase || hModule == NULL )
	{
		if( nSize < wcslen(ModuleName_W) )
		{
			
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}

		if( nSize == wcslen(ModuleName_W) )
		{
			memcpy(lpFilename, ModuleName_W, nSize*2 );
			dwRet = nSize;
		}else
		{
			wcscpy(lpFilename, ModuleName_W);
			dwRet = wcslen(ModuleName_W) + 1;
		}

		return dwRet;
	}

	return OldGetModuleFileNameW(hModule, lpFilename, nSize );
}


DWORD 
WINAPI 
FakeGetModuleFileNameA(
					   __in          HMODULE hModule,
					   __out         LPSTR lpFilename,
					   __in          DWORD nSize
					   )
{



	DWORD dwRet = 0;
	//__asm int 3
	if( hModule == (HMODULE)g_ImageBase || hModule == NULL )
	{
		if( nSize < strlen(ModuleName_A) )
		{

			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}

		if( nSize == strlen(ModuleName_A) )
		{
			memcpy(lpFilename, ModuleName_A, nSize );
			dwRet = nSize;
		}else
		{
			strcpy(lpFilename, ModuleName_A);
			dwRet = strlen(ModuleName_A) + 1;
		}

		return dwRet;
	}

	return OldGetModuleFileNameA(hModule, lpFilename, nSize );

}



DWORD 
WINAPI 
FakeFormatMessageA(
		__in          DWORD dwFlags,
		__in          LPCVOID lpSource,
		__in          DWORD dwMessageId,
		__in          DWORD dwLanguageId,
		__out         LPSTR lpBuffer,
		__in          DWORD nSize,
		__in          va_list* Arguments
		)
{
	LPCVOID lpNewSource = lpSource;
	
	if( lpSource == NULL && dwFlags & FORMAT_MESSAGE_FROM_HMODULE && hMuiLoad )
	{
		lpNewSource = hMuiLoad;
	}
	
	return OldFormatMessageA( dwFlags,
		  lpNewSource,
		  dwMessageId,
		  dwLanguageId,
		  lpBuffer,
		  nSize,
		  Arguments
		  );

}

DWORD 
WINAPI 
FakeFormatMessageW(
	 __in          DWORD dwFlags,
	 __in          LPCVOID lpSource,
	 __in          DWORD dwMessageId,
	 __in          DWORD dwLanguageId,
	 __out         LPWSTR lpBuffer,
	 __in          DWORD nSize,
	 __in          va_list* Arguments
				   )
{
	LPCVOID lpNewSource = lpSource;
	
	if( lpSource == NULL && dwFlags & FORMAT_MESSAGE_FROM_HMODULE && hMuiLoad)
	{
		lpNewSource = hMuiLoad;
	}
	
	return OldFormatMessageW( dwFlags,
		lpNewSource,
		dwMessageId,
		dwLanguageId,
		lpBuffer,
		nSize,
		Arguments
		);
}



BOOLEAN LoaderApiHook()
{
	PVOID FucAddress = NULL;

	
	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "GetModuleFileNameA" );
	if( FucAddress != NULL )
	{
		OldGetModuleFileNameA = (pfnGetModuleFileNameA)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetModuleFileNameA );
	}

	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "GetModuleFileNameW" );
	if( FucAddress != NULL )
	{
		OldGetModuleFileNameW = (pfnGetModuleFileNameW)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetModuleFileNameW );
	}
	
	FucAddress = (PVOID)DetourFindFunction( "KERNELBASE.dll" , "GetModuleFileNameA" );
	if( FucAddress != NULL )
	{
		OldGetModuleFileNameA = (pfnGetModuleFileNameA)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetModuleFileNameA );
	}

	FucAddress = (PVOID)DetourFindFunction( "KERNELBASE.dll" , "GetModuleFileNameW" );
	if( FucAddress != NULL )
	{
		OldGetModuleFileNameW = (pfnGetModuleFileNameW)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetModuleFileNameW );
	}


	
	FucAddress = (PVOID)DetourFindFunction( "msvcrt.dll" , "__getmainargs" );
	if( FucAddress != NULL )
	{
		Old__getmainargs = (pfn__getmainargs)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)Fake__getmainargs );	

	}

	FucAddress = (PVOID)DetourFindFunction( "msvcrt.dll" , "__wgetmainargs" );
	if( FucAddress != NULL )
	{
		Old__wgetmainargs = (pfn__wgetmainargs)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)Fake__wgetmainargs );	

	}
	

	
	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "GetCommandLineW" );
	if( FucAddress != NULL )
	{
		OldGetCommandLineW = (pfnGetCommandLineW)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetCommandLineW );	
		
	}

	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "GetCommandLineA" );
	if( FucAddress != NULL )
	{
		OldGetCommandLineA = (pfnGetCommandLineA)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetCommandLineA );


	}
		
	FucAddress = (PVOID)DetourFindFunction( "KERNELBASE.dll" , "GetCommandLineA" );
	if( FucAddress != NULL )
	{
		OldGetCommandLineA = (pfnGetCommandLineA)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetCommandLineA );

	}
	
	FucAddress = (PVOID)DetourFindFunction( "KERNELBASE.dll" , "GetCommandLineW" );
	if( FucAddress != NULL )
	{
		OldGetCommandLineW = (pfnGetCommandLineW)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeGetCommandLineW );	

	}
	

	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "FormatMessageW" );
	if( FucAddress != NULL )
	{
		OldFormatMessageW = (pfnFormatMessageW)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeFormatMessageW );
	}

	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "FormatMessageA" );
	if( FucAddress != NULL )
	{
		OldFormatMessageA = (pfnFormatMessageA)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeFormatMessageA);
	}


	FucAddress = (PVOID)DetourFindFunction( "msvcrt.dll" , "exit" );  ////
	if( FucAddress != NULL )
	{
		Oldexit = (pfnexit)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)Fakeexit );
	}

	FucAddress = (PVOID)DetourFindFunction( "kernel32.dll" , "ExitProcess" );
	if( FucAddress != NULL )
	{
		OldExitProcess = (pfnExitProcess)DetourFunction( (PBYTE)FucAddress, 
			(PBYTE)FakeExitProcess );
	}

	//////////////////////////////////////////////////////////////////////////
	return TRUE;
}

void LoaderApiRemoveHook()
{
	DetourRemove((PBYTE)OldFormatMessageW, (PBYTE)FakeFormatMessageW);
	DetourRemove((PBYTE)OldFormatMessageA, (PBYTE)FakeFormatMessageA);
	DetourRemove((PBYTE)OldGetModuleFileNameA, (PBYTE)FakeGetModuleFileNameA);
	DetourRemove((PBYTE)OldGetModuleFileNameW, (PBYTE)FakeGetModuleFileNameW);
	DetourRemove((PBYTE)OldGetCommandLineW, (PBYTE)FakeGetCommandLineW);
	DetourRemove((PBYTE)OldGetCommandLineA, (PBYTE)FakeGetCommandLineA );
	DetourRemove((PBYTE)OldExitProcess, (PBYTE)FakeExitProcess );
	DetourRemove((PBYTE)Oldexit, (PBYTE)Fakeexit );
	DetourRemove((PBYTE)Old__getmainargs, (PBYTE)Fake__getmainargs );
	DetourRemove((PBYTE)Old__wgetmainargs, (PBYTE)Fake__wgetmainargs );


}



void FuckModulePath(LPWSTR PathW, LPSTR PathA )
{
	ZeroMemory(ModuleName_W, MAX_PATH*sizeof(WCHAR));
	ZeroMemory(ModuleName_A, MAX_PATH*sizeof(CHAR));

	if( PathW )
		StringCchCopyW(ModuleName_W, MAX_PATH, PathW );
	if(PathA)
		StringCchCopyA(ModuleName_A, MAX_PATH, PathA );

}

void FuckCommadLine(LPWSTR CmdW, LPSTR CmdA)
{
	ZeroMemory(CMDLINE_W, MAX_PATH*sizeof(WCHAR));
	ZeroMemory(CMDLINE_A, MAX_PATH*sizeof(CHAR));

	if( CmdW )
	{
		
		StringCchCopyW(CMDLINE_W, MAX_PATH, CmdW );
	}
	if(CmdA )
		StringCchCopyA(CMDLINE_A, MAX_PATH, CmdA );
}


void SuckGetCommandLine(IN char *NewCmdLine )
{
	WCHAR wCmdLine[MAX_PATH+10];
	if( NewCmdLine )
	{
		int nLength = 0;
		nLength = MultiByteToWideChar(CP_ACP,0,NewCmdLine,-1,NULL,0);
		MultiByteToWideChar(CP_ACP,0,NewCmdLine,-1, wCmdLine, nLength);
		FuckCommadLine(wCmdLine, NewCmdLine);
	}

}

void SuckGetModuleFileName(IN char* NewFilePath )
{
	WCHAR wNewFilePath[MAX_PATH+10];
	if( NewFilePath )
	{
		int nLength = 0;
		nLength = MultiByteToWideChar(CP_ACP,0,NewFilePath,-1,NULL,0);
		MultiByteToWideChar(CP_ACP,0,NewFilePath,-1, wNewFilePath, nLength);

		FuckModulePath(wNewFilePath, NewFilePath );
	}

}


//
//eg: FilePath C:\\WINDOWS\\system32\\ping.exe
//    Cmdline wwww.baidu.com
//
HMODULE gModule = NULL;

BOOLEAN
LoaderRun(char* FilePath, char* Cmdline = NULL )
{
	CHAR NewCmdLine[MAX_PATH+10];

	if( Cmdline )
	{
		StringCchCopyA(NewCmdLine, MAX_PATH+10, FilePath );
		StringCchCatA(NewCmdLine, MAX_PATH+10, "  ");
		StringCchCatA(NewCmdLine, MAX_PATH+10, Cmdline );


		SuckGetCommandLine(NewCmdLine);
		SuckGetModuleFileName(NewCmdLine);

	}else
	{
		SuckGetCommandLine(FilePath);
		SuckGetModuleFileName(FilePath);
	}

	HMODULE hModule;
	hModule = LoadExecutable( FilePath );
	if( hModule )
	{
		gModule = hModule;
		CallLoadedImage( hModule  );	
	}

	return TRUE;

}


DWORD WINAPI RunningThread( LPVOID lpParam ) 
{ 
	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)lpParam;
	PIMAGE_FILE_HEADER pfh = (PIMAGE_FILE_HEADER)( (DWORD)lpParam + dos->e_lfanew + 4 );
	PIMAGE_OPTIONAL_HEADER poh = (PIMAGE_OPTIONAL_HEADER)( pfh+1 );

	if( poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress )
	{
		PIMAGE_TLS_DIRECTORY pTlsDir = (PIMAGE_TLS_DIRECTORY) RVATOVA(poh->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress, (HMODULE)lpParam);
		LdrInitThreadTls(pTlsDir);
		_LdrpCallTlsInitializers( (HANDLE)lpParam, pTlsDir, 0 );
	}

	//__asm int 3
	CallLoadedImage( (HMODULE)lpParam  );
	return 0; 
} 


void
RunFile(char* FilePath, char* Cmdline = NULL )
{
	DWORD dwThreadId = 0;
	HANDLE hThread = NULL;

	CHAR NewCmdLine[MAX_PATH+10];

	ZeroMemory(ModuleName_W, MAX_PATH*sizeof(WCHAR));
	ZeroMemory(ModuleName_A, MAX_PATH*sizeof(CHAR));


	ZeroMemory(CMDLINE_W, MAX_PATH*sizeof(WCHAR));
	ZeroMemory(CMDLINE_A, MAX_PATH*sizeof(CHAR));

	if( Cmdline )
	{
		StringCchCopyA(NewCmdLine, MAX_PATH+10, FilePath );
		StringCchCatA(NewCmdLine, MAX_PATH+10, "  ");
		StringCchCatA(NewCmdLine, MAX_PATH+10, Cmdline );

		printf("\n\n\n******** %s\n", FilePath );

		SuckGetCommandLine(NewCmdLine);
		SuckGetModuleFileName(NewCmdLine);

	}else
	{
		
		SuckGetCommandLine(FilePath);
		SuckGetModuleFileName(FilePath);
	}

	
	printf("COMMAND LINE %s\n", CMDLINE_A );

	HMODULE hModule;
	hModule = LoadExecutable( FilePath );
	if( hModule )
	{
		LoaderApiHook();
		hThread = CreateThread( 
			NULL,              // default security attributes
			0,                 // use default stack size  
			RunningThread,          // thread function 
			(PVOID)hModule,             // argument to thread function 
			0,                 // use default creation flags 
			&dwThreadId);   // returns the thread identifier 
		WaitForSingleObject(hThread, INFINITE );

		CloseHandle(hThread);
	}

}



BOOLEAN
LdrFindFile(
	IN LPSTR FileName,
	OUT LPSTR FindPath,
	IN ULONG Cch
	)
{
	BOOLEAN bResult = FALSE;

	if( FileName == NULL || FindPath == NULL || Cch == 0 )
		goto _EXIT0;

	LPSTR lpFilePart = NULL;
	DWORD dwResult = SearchPathA(
		NULL,
		FileName,
		".exe",	
		Cch,
		FindPath,
		&lpFilePart
		);
	if(dwResult != 0 )
	{
		bResult = TRUE;
	}

_EXIT0:
	return bResult;
}


BOOLEAN 
LdrParse( 
	IN LPSTR pszInput, 
	OUT LPSTR FilePath, 
	IN ULONG CchFilePath,
	OUT LPSTR CmdLine, 
	IN ULONG CchCmdLine
	)
{

	BOOLEAN bResult = FALSE;

	if( pszInput == NULL || FilePath == NULL || CmdLine == NULL )
		goto _EXIT0;

	BOOLEAN bParent = FALSE;
	CHAR FileName[MAX_PATH] = {0};
	int j = 0, i = 0;

	if( pszInput[0] == '"' )
	{
		bParent = TRUE;
		i++;
	}

	ZeroMemory(CmdLine, CchCmdLine);
	for( i ; i < strlen(pszInput); i++ )
	{
		
		if( (pszInput[i] == ' ' && bParent == FALSE) || 
			(bParent == TRUE && pszInput[i] == '"')
			)
		{
			StringCchCopyA(CmdLine, CchCmdLine, &pszInput[i+1]);
			break;
		}

		FileName[j++] = pszInput[i];
	}

	bResult = LdrFindFile(FileName, FilePath, CchFilePath);

_EXIT0:
	return bResult;
}

HMODULE
LdrLoadMuiFile(
	IN LPSTR FilePath
	)
{

	HMODULE hModule = NULL;
	typedef BOOL (WINAPI *__pfnGetFileMUIPath)(
		DWORD  dwFlags,
		PCWSTR  pcwszFilePath,
		PWSTR  pwszLanguage,
		PULONG  pcchLanguage,
		PWSTR  pwszFileMUIPath,
		PULONG  pcchFileMUIPath,
		PULONGLONG  pululEnumerator
		);

	if( FilePath == NULL )
		goto _EXIT0;

	__pfnGetFileMUIPath pfnGetFileMUIPath = NULL;

	pfnGetFileMUIPath = (__pfnGetFileMUIPath)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetFileMUIPath" );
	if( pfnGetFileMUIPath == NULL )
		goto _EXIT0;

	WCHAR wFilePath[MAX_PATH];
	WCHAR wMuiFilePath[MAX_PATH];

	int nLength = 0;
	nLength = MultiByteToWideChar(CP_ACP, 0, FilePath, -1, NULL, 0);
	MultiByteToWideChar(CP_ACP, 0, FilePath, -1, wFilePath, nLength);

	ULONG  pcchFileMUIPath = MAX_PATH;
	ULONGLONG  pululEnumerator = 0;

	if( pfnGetFileMUIPath(MUI_LANGUAGE_NAME, 
		wFilePath,
		NULL,
		0,
		wMuiFilePath,
		&pcchFileMUIPath,
		&pululEnumerator
		) == TRUE )
	{
		printf("**** mui file %ws\n", wMuiFilePath );
		hModule = LoadLibraryExW(wMuiFilePath, NULL, NULL);
	}

_EXIT0:
	return hModule;
}

BOOL FreeLoaderModules();
BOOLEAN RunFile2(IN LPSTR Input)
{
	BOOLEAN bResult = FALSE;
	LPSTR pcmdLine = NULL;
	CHAR FilePath[MAX_PATH] = {0 }, CmdLine[MAX_PATH] = {0};
	
	if( LdrParse(Input, FilePath, MAX_PATH, CmdLine, MAX_PATH) == FALSE )
		goto _EXIT0;

	if( strlen(CmdLine) )
		pcmdLine = CmdLine;


	//
	//load mui
	//

	hMuiLoad = LdrLoadMuiFile(FilePath);

	RunFile( FilePath , pcmdLine);

	if( g_Argcx )
	{
		LocalFree(g_Argcx);
		g_Argcx = NULL;
	}
	if( g_w_Argcx)
	{
		LocalFree(g_w_Argcx);
		g_w_Argcx = NULL;
	}

	UnloadExecutable( (HMODULE)g_ImageBase );

	if( hMuiLoad )
		FreeLibrary((HMODULE)hMuiLoad);


	//FreeLoaderModules();


	LoaderApiRemoveHook();

_EXIT0:
	return bResult;
}



//
//BOOLEAN RunFile2(IN LPSTR Input)
//{
//	return TRUE;
//}

//
//先要禁止 msvcrt加载，后面LOAD这个DLL的时候才会初始话，才会调用GetCommandLine，否则无法捕获到参数
//
/*
WIN 7 ARP.EXE
.text:0100239E                 push    dword_1005968
.text:010023A4                 push    dword_1005960
.text:010023AA                 call    _main


msvcrt.dll 第一次被加载的时候，会调用GetCommandLine来获取当前参数，然后进行处理填充msvcrt的内部变量	
msvcrt!__argc 和 msvcrt!__argv。之后通过__getmainargs, __wgetmainargs就可以获取到相应参数，这时候并不会再走GetCommandLine。WIN 7的ARP.EXE获取命令行参数就是通过__getmainargs。因为msvcrt.dll在进程运行的时候已经被加载，所以无法拦截到GetCommandLine


俩个参数在LOAD msvcrt.dll时被初始化。。WIN 7每个进程一运行就会加载这个DLL。。。日啊，没时机捕获参数
*/
/*
#pragma comment(linker, "/INCLUDE:__tls_used")
void NTAPI my_tls_callback(PVOID h, DWORD reason, PVOID pv)
{
	
	//仅在进程初始化创建主线程时执行的代码
	if( reason == DLL_PROCESS_ATTACH ){
		//__asm int 3

		//MmVirtualAlloc( (PVOID)0x400000, 0x100000); // 保留1M
		PVOID pResult = VirtualAlloc( (PVOID)0x400000, 
			0x1000*12, 
			MEM_RESERVE, // 保留空间 和提交
			PAGE_EXECUTE_WRITECOPY
			);
		//if( pResult != (PVOID)0x400000 )
		//{
		//	VirtualFree( pResult, 0, MEM_RELEASE);
		//}

		//MessageBoxW(NULL,L"hi,this is tls callback",L"title",MB_OK);
	}
	return;
}
*/
//#pragma data_seg(".CRT$XLB")
/*如果要定义多个TLS_CallBack函数，可以把下面这句写成：
PIMAGE_TLS_CALLBACK p_thread_callback [] = {tls_callback_A, tls_callback_B, tls_callback_C,0};
其中tls_callback_B和tls_callback_C应该是你定义好的其他TLS_callBack函数
*/

/*
PIMAGE_TLS_CALLBACK p_thread_callback = my_tls_callback;
#pragma data_seg()
*/




BOOL DoMakeMemoryInitOk()
{

	LPVOID pAllocate = NULL;
	BOOL bMem0Ok = FALSE, bMem1Ok = FALSE;
	MEMORY_BASIC_INFORMATION lpBuffer = {0 };

	VirtualQuery((PVOID)g_ImageBase0, 
		&lpBuffer,
		sizeof(lpBuffer)
		);
	if(  lpBuffer.Protect == PAGE_EXECUTE_READWRITE &&
		((UCHAR*)g_ImageBase0)[0] == 'S' && 
		((UCHAR*)g_ImageBase0)[1] == 'y' &&
		((UCHAR*)g_ImageBase0)[2] == 's' &&
		((UCHAR*)g_ImageBase0)[3] == 'n' &&
		((UCHAR*)g_ImageBase0)[4] == 'a' &&
		((UCHAR*)g_ImageBase0)[5] == 'p' 
		)
	{
		bMem0Ok = TRUE;
	}

	VirtualQuery((PVOID)g_ImageBase1, 
		&lpBuffer,
		sizeof(lpBuffer)
		);
	if(  lpBuffer.Protect == PAGE_EXECUTE_READWRITE &&
		((UCHAR*)g_ImageBase1)[0] == 'S' && 
		((UCHAR*)g_ImageBase1)[1] == 'y' &&
		((UCHAR*)g_ImageBase1)[2] == 's' &&
		((UCHAR*)g_ImageBase1)[3] == 'n' &&
		((UCHAR*)g_ImageBase1)[4] == 'a' &&
		((UCHAR*)g_ImageBase1)[5] == 'p' 
		)
	{
		bMem1Ok = TRUE;
	}

	if( bMem0Ok == FALSE  )
	{
		
		pAllocate = VirtualAlloc( (PVOID)g_ImageBase0, 
			g_ImageSize, 
			MEM_COMMIT|MEM_RESERVE, // 保留空间 和提交
			PAGE_EXECUTE_READWRITE
			);
		if( pAllocate == (PVOID)g_ImageBase0 )
		{
			bMem0Ok = TRUE;
		}
	}

	if( bMem1Ok == FALSE  )
	{
		
		pAllocate = VirtualAlloc( (PVOID)g_ImageBase1, 
			g_ImageSize, 
			MEM_COMMIT|MEM_RESERVE, // 保留空间 和提交
			PAGE_EXECUTE_READWRITE
			);
		if( pAllocate == (PVOID)g_ImageBase1 )
		{
			bMem1Ok = TRUE;
		}
	}

	if( bMem0Ok == FALSE || bMem1Ok == FALSE )
	{
	
		//
		//SUSPEN AND STSRT 
		//

		CHAR MySelfPath[MAX_PATH] = {0 };
		GetModuleFileNameA(0, MySelfPath, MAX_PATH);

		STARTUPINFOA si;
		PROCESS_INFORMATION pi;

		ZeroMemory( &si, sizeof(si) );
		si.cb = sizeof(si);
		ZeroMemory( &pi, sizeof(pi) );

		// Start the child process. 
		if( CreateProcessA( NULL,   // No module name (use command line)
			MySelfPath,        // Command line
			NULL,           // Process handle not inheritable
			NULL,           // Thread handle not inheritable
			TRUE,          // Set handle inheritance to FALSE
			CREATE_SUSPENDED,              // No creation flags
			NULL,           // Use parent's environment block
			NULL,           // Use parent's starting directory 
			&si,            // Pointer to STARTUPINFO structure
			&pi )           // Pointer to PROCESS_INFORMATION structure
			) 
		{

			SIZE_T lpNumberOfBytesWritten = 0;
			UCHAR pWBuffer[8] = "Sysnap";

			pAllocate = VirtualAllocEx(pi.hProcess, 
				(PVOID)g_ImageBase0,
				g_ImageSize, 
				MEM_COMMIT|MEM_RESERVE, // 保留空间 和提交
				PAGE_EXECUTE_READWRITE
				);
			if( pAllocate == (PVOID)g_ImageBase0 )
			{
				WriteProcessMemory(pi.hProcess, 
					pAllocate,
					pWBuffer,
					8,
					&lpNumberOfBytesWritten
					);
			}

			pAllocate = VirtualAllocEx(pi.hProcess, 
				(PVOID)g_ImageBase1,
				g_ImageSize, 
				MEM_COMMIT|MEM_RESERVE, // 保留空间 和提交
				PAGE_EXECUTE_READWRITE
				);
			if( pAllocate == (PVOID)g_ImageBase1 )
			{
				WriteProcessMemory(pi.hProcess, 
					pAllocate,
					pWBuffer,
					8,
					&lpNumberOfBytesWritten
					);
			}

			ResumeThread(pi.hThread );
			CloseHandle( pi.hProcess );
			CloseHandle( pi.hThread );

		}


		ExitProcess(0);
	}
	
	//MmVirtualFree(g_ImageBase0, g_ImageSize);
	//MmVirtualFree(g_ImageBase1, g_ImageSize);
	bMemReservOk = TRUE;

	//
	//不要FREE，保证内存一直占有
	//
	//__asm int 3

	//if( g_ImageBase0 )
	//	VirtualFree( g_ImageBase0, g_ImageSize, MEM_RELEASE);

	//if( g_ImageBase1 )
	//	VirtualFree( g_ImageBase1, g_ImageSize, MEM_RELEASE);


	return TRUE;
}

/*
ULONG gLoaderInitModulesCounts = 0;
ULONG gLoaderInitModules[100] = { 0 };





BOOL TakeLoaderModulesSnap(  )
{
	HANDLE hModuleSnap = INVALID_HANDLE_VALUE;
	MODULEENTRY32 me32;

	hModuleSnap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE, GetCurrentProcessId() );
	if( hModuleSnap == INVALID_HANDLE_VALUE )
	{
		return( FALSE );
	}

	me32.dwSize = sizeof( MODULEENTRY32 );

	if( !Module32First( hModuleSnap, &me32 ) )
	{
		CloseHandle( hModuleSnap );    // Must clean up the
		return( FALSE );
	}

	do
	{

		gLoaderInitModules[gLoaderInitModulesCounts++] = (ULONG)me32.modBaseAddr;

	} while( Module32Next( hModuleSnap, &me32 ) );

	CloseHandle( hModuleSnap );
	return( TRUE );
}

BOOL FreeLoaderModules()
{
	HANDLE hModuleSnap = INVALID_HANDLE_VALUE;
	MODULEENTRY32 me32;

	hModuleSnap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE, GetCurrentProcessId() );
	if( hModuleSnap == INVALID_HANDLE_VALUE )
	{
		return( FALSE );
	}

	me32.dwSize = sizeof( MODULEENTRY32 );

	if( !Module32First( hModuleSnap, &me32 ) )
	{
		CloseHandle( hModuleSnap );    // Must clean up the
		return( FALSE );
	}

	BOOLEAN bFind = FALSE;
	do
	{
		
		for( ULONG i = 0; i< gLoaderInitModulesCounts; i++ )
		{
			if( gLoaderInitModules[i] == (ULONG)me32.modBaseAddr )
				bFind = TRUE;
		}

		
		if( bFind == FALSE )
			FreeLibrary((HMODULE)me32.modBaseAddr);

		bFind = FALSE;
		
		

	} while( Module32Next( hModuleSnap, &me32 ) );

	CloseHandle( hModuleSnap );
	return( TRUE );
}
*/

int main(int argc, char *argv[])
{

	CHAR Cmd[260];

	//gLoaderInitModules[0] = 0;
	//return 0;
	//TakeLoaderModulesSnap();
	DoMakeMemoryInitOk();


	//__asm int 3

	while(1)
	{
		printf("\ninput cmd :>  ");
		gets(Cmd);
		//scanf("%s", Cmd);
		RunFile2(Cmd);
	}

	return 0;
	//LoaderApiHook();

	//__asm int 3
	//LPBYTE Mapping = (LPBYTE)MmVirtualAlloc( (PVOID)0x400000, 0x100 );
	//if( Mapping == NULL )
	//{
	//	printf("failed\n");
	//}


	//RunFile2("cacls");
	//RunFile2("net help");

	//LoadLibraryA("shell32.dll");
	////RunFile2("ping www.baidu.com");

	//RunFile2("sc qc msdtc ");

	//////RunFile( "shutdown.exe /?" );

	//return 0;

	//RunFile2("ipconfig /all");

	if( argc != 2 )
	{
		printf("error input!\n");
		return 0;
	}

	RunFile2(argv[1]);

	ExitProcess(0xbeebee);
	//RunFile2("ping www.baidu.com");

	//return 0;

	//RunFile2("ping www.baidu.com");

	return  0;

	RunFile2("arp -a");

	RunFile2("cacls");
	RunFile2("net help");
	//RunFile2("tasklist.exe ");

	//RunFile( "C:\\WINDOWS\\regedit.exe" );


	//exit(0xbeebee);
	return 1;


	//LoaderRun("C:\\WINDOWS\\system32\\tasklist.exe");
	//RunFile( "C:\\WINDOWS\\system32\\net.exe" , " user");
	//RunFile( "C:\\WINDOWS\\system32\\arp.exe" );
	//RunFile( "C:\\WINDOWS\\system32\\ipconfig.exe" , " /all");
	//RunFile( "C:\\PEview.exe" );
	//RunFile("C:\\WINDOWS\\system32\\mspaint.exe",  NULL );
	//RunFile( "C:\\WINDOWS\\regedit.exe" );
	//RunFile( "C:\\WINDOWS\\system32\\cleanmgr.exe" );

	//ExitProcess(0);


	return 0;


}

