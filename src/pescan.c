/*
	pev - the PE file analyzer toolkit
	
	pescan.c - search for suspicious things in PE files

	Copyright (C) 2012 Fernando Mercês

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "pescan.h"

static int ind;

void usage()
{
	printf("Usage: %s OPTIONS FILE\n"
	"Search for genereic packers in PE files\n"
	"\nExample: %s putty.exe\n"
	"\nOptions:\n"
	" -o, --offsets                          show things offsets when possible\n"
	" -v, --version                          show version and exit\n"
	" --help                                 show this help and exit\n",
	PROGRAM, PROGRAM);
}

void parse_options(int argc, char *argv[])
{
	int c;

	/* Parameters for getopt_long() function */
	static const char short_options[] = "vo";

	static const struct option long_options[] = {
		{"offsets",          no_argument,       NULL, 'o'},
		{"help",             no_argument,       NULL,  1 },
		{"version",          no_argument,       NULL, 'v'},
		{ NULL,              0,                 NULL,  0 }
	};
	
	memset(&config, 0, sizeof(config));

	while ((c = getopt_long(argc, argv, short_options,
			long_options, &ind)))
	{
		if (c < 0)
			break;

		switch (c)
		{
			case 1:		// --help option
				usage();
				exit(EXIT_SUCCESS);
				
			case 'o':
				config.show_offsets = true;
				break;
				
			case 'v':
				printf("%s %s\n%s\n", PROGRAM, TOOLKIT, COPY);
				exit(EXIT_SUCCESS);

			default:
				fprintf(stderr, "%s: try '--help' for more information\n", PROGRAM);
				exit(EXIT_FAILURE);
		}
	}
}

// check for abnormal dos stub (common in packed files)
bool abnormal_dos_stub(PE_FILE *pe, DWORD *stub_offset)
{
   BYTE dos_stub[] =
   "\x0e"               // push cs
   "\x1f"               // pop ds
   "\xba\x0e\x00"       // mov dx, 0x0e
   "\xb4\x09"           // mov ah, 0x09
   "\xcd\x21"           // int 0x21
   "\xb8\x01\x4c"       // mov ax, 0x4c01
   "\xcd\x21"           // int 0x21
   "This program cannot be run in DOS mode.\r\r\n$";

   BYTE data[sizeof(dos_stub)-1]; // -1 to ignore ending null
   IMAGE_DOS_HEADER dos;


   if (!pe_get_dos(pe, &dos))
      EXIT_ERROR("unable to retrieve PE DOS header");
	
	*stub_offset = dos.e_cparhdr << 4;

   // dos stub starts at e_cparhdr shifted by 4
   if (fseek(pe->handle, *stub_offset, SEEK_SET))
      EXIT_ERROR("unable to seek in file");

   if (!fread(&data, sizeof(data), 1, pe->handle))
      EXIT_ERROR("unable to read DOS stub");
	
   if (memcmp(dos_stub, data, sizeof(data)))
      return true;

   return false;
}

IMAGE_SECTION_HEADER *pe_check_fake_entrypoint(PE_FILE *pe, DWORD *ep)
{
	IMAGE_SECTION_HEADER *epsec = NULL;

	if (!pe->optional_ptr)
		pe_get_optional(pe);

	if (!pe->num_sections || !pe->sections_ptr)
		pe_get_sections(pe);

	if (!pe->num_sections)
		return NULL;

	epsec = pe_rva2section(pe, *ep);

	if (!epsec)
		return NULL;

	if (!(epsec->Characteristics & 0x20))
		return epsec;

   return NULL;
}

DWORD pe_get_tls_directory(PE_FILE *pe)
{
	if (!pe_get_directories(pe))
		return 0;

	if (pe->num_directories > 32)
		return 0;

	for (unsigned int i=0; (i < pe->num_directories && pe->directories_ptr[i]); i++)
	{
		if ((i == IMAGE_DIRECTORY_ENTRY_TLS) && pe->directories_ptr[i]->Size > 0)
			return pe->directories_ptr[i]->VirtualAddress;
	}
	return 0;
}

/*
 * -1 - fake tls callbacks detected
 *  0 - no tls directory
 * >0 - number of callbacks functions found
*/
int pe_get_tls_callbacks(PE_FILE *pe)
{
	QWORD tls_addr = 0;
	int ret = 0;
	
	if (!pe)
		return 0;

	tls_addr = pe_get_tls_directory(pe);
		
	if (!tls_addr || !pe_get_sections(pe))
		return 0;


	// search for tls in all sections
	for (unsigned int i=0, j=0; i < pe->num_sections; i++)
	{
		if (tls_addr >= pe->sections_ptr[i]->VirtualAddress &&
		tls_addr < (pe->sections_ptr[i]->VirtualAddress + pe->sections_ptr[i]->SizeOfRawData))
		{
			unsigned int funcaddr = 0;

			if (fseek(pe->handle, tls_addr - pe->sections_ptr[i]->VirtualAddress
			+ pe->sections_ptr[i]->PointerToRawData, SEEK_SET))
			 	return 0;

			if (pe->architecture == PE32)
			{
				IMAGE_TLS_DIRECTORY32 tlsdir32;

				if (!fread(&tlsdir32, sizeof(tlsdir32), 1, pe->handle))
					return 0;	

				if (! (tlsdir32.AddressOfCallBacks & pe->optional_ptr->_32->ImageBase))
					break;

				if (fseek(pe->handle,
						rva2ofs(pe, tlsdir32.AddressOfCallBacks - pe->optional_ptr->_32->ImageBase), SEEK_SET))
					return 0;
			}
			else if (pe->architecture == PE64)
			{
				IMAGE_TLS_DIRECTORY64 tlsdir64;

				if (!fread(&tlsdir64, sizeof(tlsdir64), 1, pe->handle))
					return 0;	

				if (! (tlsdir64.AddressOfCallBacks & pe->optional_ptr->_64->ImageBase))
					break;

				if (fseek(pe->handle,
				 rva2ofs(pe, tlsdir64.AddressOfCallBacks - pe->optional_ptr->_64->ImageBase), SEEK_SET))
					return 0;
			}
			else
				return 0;

			ret = -1; // tls directory and section exists
			do
			{ 
				fread(&funcaddr, sizeof(int), 1, pe->handle);
				if (funcaddr)
				{
					char value[MAX_MSG];

					ret = ++j; // function found
					
					if (config.show_offsets)
					{
						snprintf(value, MAX_MSG, "%#x", funcaddr);
						output("TLS callback function", value);
					}
				}
			} while (funcaddr);

			return ret;
		}
	}
	return 0;
}

/* TODO
  25 secoes com caracteres nao imprimiveis
 26 muitas secoes
 27 nenhuma secao
 28 entrypoint nulo
 29 timestamp invalido
 30 sem imagebase
 31 diretorios sem tamanho
*/

int main(int argc, char *argv[])
{
	PE_FILE pe;
	FILE *fp = NULL;
	DWORD ep, stub_offset;
	char value[MAX_MSG];
	int callbacks;

	if (argc < 2)
	{
		usage();
		exit(1);
	}

	parse_options(argc, argv); // opcoes

	if ((fp = fopen(argv[argc-1], "rb")) == NULL)
		EXIT_ERROR("file not found or unreadable");

	pe_init(&pe, fp); // inicializa o struct pe

	if (!ispe(&pe))
		EXIT_ERROR("not a valid PE file");

	if (!pe_get_optional(&pe))
		return 1;

   ep = (pe.optional_ptr->_32 ? pe.optional_ptr->_32->AddressOfEntryPoint :
	(pe.optional_ptr->_64 ? pe.optional_ptr->_64->AddressOfEntryPoint : 0));

	if (!ep)
		return 1;

	// fake ep
	if (pe_check_fake_entrypoint(&pe, &ep))
		if (config.show_offsets)
			snprintf(value, MAX_MSG, "fake - va: %#x - raw: %#lx", ep, rva2ofs(&pe, ep));
		else
			snprintf(value, MAX_MSG, "fake");
	else
		if (config.show_offsets)
			snprintf(value, MAX_MSG, "normal - va: %#x - raw: %#lx", ep, rva2ofs(&pe, ep));
		else
			snprintf(value, MAX_MSG, "normal");
		
	output("entrypoint", value);

	// dos stub
	memset(&value, 0, sizeof(value));
	if (abnormal_dos_stub(&pe, &stub_offset))
	{
		if (config.show_offsets)
			snprintf(value, MAX_MSG, "suspicious - raw: %#x", stub_offset);
		else
			snprintf(value, MAX_MSG, "suspicious");
	}
	else
		snprintf(value, MAX_MSG, "normal");
	
	output("DOS stub", value);
	
	callbacks = pe_get_tls_callbacks(&pe);
	
	if (callbacks == 0)
		snprintf(value, MAX_MSG, "not found");
	else if (callbacks == -1)
		snprintf(value, MAX_MSG, "found - no functions");
	else if (callbacks >0)
		snprintf(value, MAX_MSG, "found - %d function(s)", callbacks);
	
	output("TLS directory", value);

	pe_deinit(&pe);
	return 0;
}
