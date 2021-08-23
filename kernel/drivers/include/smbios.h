#ifndef SMBIOS_H
#define SMBIOS_H
#include <stdint.h>
// Entry Point Table of SMBIOS
typedef struct 
{
    char anchor_string[4]; // _SM_, specified as four ASCII characters(5F 53 4D 4F);
    uint8_t checksum; // Checksum of the Entry Point Table, should be 0 (overflow);
    uint8_t length; // Length of the Entry Point Table . Since version 2.1 of SMBIOS, this is 1Fh
    uint8_t major_version; // Major version of SMBIOS;
    uint8_t minor_version; // Minor version of SMBIOS;
    uint16_t max_structure_size; // Maximum size of a SMBIOS Structure;
    uint8_t  entry_point_revision; //  EPS revision implemented in this structure;
    char formated_area[5]; // ...
    char entry_point_string[5]; /// This is _DMI_;
    uint8_t checksum2; // Checksum for values from entry_point_string to the end of table;
    uint16_t table_length; // Length of the Table;
    uint32_t table_address; // Address of the Table;
    uint16_t number_of_structures; // Number of structures in the table;
    uint8_t bcd_revision; // Unused;
}SMBIOS_Entry_Point_t;
// SMBIOS Header
typedef struct 
{
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} SMBIOS_Header_t;


#endif