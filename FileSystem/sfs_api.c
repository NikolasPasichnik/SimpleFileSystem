#include "sfs_api.h"
#include<stdio.h>
#include<stdlib.h> 
#include<unistd.h>
#include<stdint.h>
#include<string.h>
#include "disk_emu.h"

#define BLOCK_SIZE 1024
#define MAX_BLOCK 1024 

/*
Notes: 
1. MAX_FNAME_LENGTH = 15 //The code was built with the assumption that files of size 15 + '\0' will be used
2. MAX_FD = 10 //While more than 10 files can be created, only 10 of them can be opened at once. 
3. MAX_BYTES = 30000 //The default value in the tests works
4. MIN_BYTES = 10000 //The default value in the tests works
5. With the i-node construction, the largest file size is 12*1024 + 1024^2/4 = 274432 bytes 
*/

struct super_node{
    int magic_number;
    int block_size; 
    int file_system_size; 
    int i_node_table_length; 
    int root_directory_node; 
};

struct i_node{
    uint32_t file_size;
    uint32_t direct_pointer[12]; //12 direct pointers
    uint32_t indirect_pointer; //1 indirect pointer block 
}; 

struct directory_entry{
    char entry_used; 
    char filename[16]; //filename + \0, max filename length is 15
    uint32_t i_node_number; 
};

struct file_descriptor_entry{
    uint32_t i_node_number; 
    uint32_t read_write_pointer; 
};

//Caches
struct i_node i_node_table[114];
struct directory_entry directory_table[96];
struct file_descriptor_entry file_descriptor_table[10]; 
char free_bit_map[1024]; 

//Pointer for sfs_getnextfilename
int current_file_read; 

void mksfs(int fresh){ 

    //Starting up the pointer for sfs_getnextfilename
    current_file_read = 0; 

    //Some arbitrary 'filename' for the disk 
    char *disk_name = "current_disk"; 

    /*
    For every slot in the FDT, i set the i_node_number attribute to -1 to indicate that it is free and no i-node 
    is stored in the given slot currently. When a file will be opened, the corresponding i_node_number will be >= 0. 
    */
    for (int i = 0; i < 10; i++){ 
        file_descriptor_table[i].i_node_number = -1; 
    }

    //Case where a new file system is requested
    if (fresh == 1){ 

        //Creating a new disk
        init_fresh_disk(disk_name, BLOCK_SIZE, MAX_BLOCK); 

        //==========================================SUPER BLOCK======================================================

        //Set up the Super Block
        struct super_node *superNode = (struct super_node*)malloc(sizeof(struct super_node));
        superNode->magic_number = 1; 
        superNode->block_size = BLOCK_SIZE; //1024 bytes per block 
        superNode->file_system_size = MAX_BLOCK; //1024 blocks in the system 
        superNode->i_node_table_length = 114; //114 files can be made at most
        superNode->root_directory_node = 0; 

        //Writing the Super Block to the disk
        write_blocks(0, 1, superNode); 

        //=========================================I-NODE TABLE======================================================

        /*
        For every slot of the I-Node table, whenever a slot is not in use, the file size will be set as -1. This will be of use
        when a new file must be created, and an I-Node must be stored. When a slot of the I-Node Table is in use, then the file_size 
        will be >= 0. 
        */
        for (int i = 0; i<114; i++){
            i_node_table[i].file_size = -1; // -1 == free | x >= 0 == used
        }

        //Creating an I-node for the root directory
        i_node_table[0].file_size = 0; 

        //Setting the pointers to -1 to indicate that they're not allocated/used yet
        for(int i = 0; i<12; i++){
            i_node_table[0].direct_pointer[i] = -1; 
        }
        i_node_table[0].indirect_pointer = -1; 

        //Writing the I-Node table to the disk at disk blocks [1, 6]
        write_blocks(1, 6, i_node_table); 

        //========================================DIRECTORY TABLE====================================================

        /* 
        For every slot of the directory table, whenever a slot is not in use, the entry_used attribute will be set to 0. This will be used 
        when a new directory is being created, and it must be stored somewhere in the table. When a slot of the Directory Table is in used, 
        the attribute entry_used will be equal 1.
        */
        for (int i = 0; i < 96; i++){
            directory_table[i].entry_used = '0'; // 0 == free | 1 == used
        }

        //Creating a directory for the root (??) 
        directory_table[0].entry_used = '1'; 
        char temp_file_name[] = "root"; 
        strcpy(directory_table[0].filename, temp_file_name); 
        directory_table[0].i_node_number = 0; 

        // Writing the Directory Table to the disk at disk blocks [7, 8]
        write_blocks(7, 2, directory_table); 

        //==========================================FREE BITMAP======================================================

        /*
        For every slot in the free bitmap, whenever a corresponding block is not in use, it will be equal to '1'. When a block is being used 
        by something, then it will be equal to '0'. 
        */
        for(int i = 0; i < 1024; i++){
            free_bit_map[i] = '1'; // '1' == free | '0' == used
        }

        //Updating Free Bitmap for Super Block [0], I-Node Table [1-6] and Directory Table [7-8] 
        for(int i = 0; i < 9; i++){
            free_bit_map[i] = '0'; 
        }
        free_bit_map[1023] = '0'; //for the Free Bitmap itself! 

        // Writing the Free Bitmap to the disk at blocks [1023]
        write_blocks(1023, 1, free_bit_map);
 
    }

    //Case where an existing file system is requested 
    else{

        //Opening existing filesystem
        init_disk(disk_name, BLOCK_SIZE, MAX_BLOCK); 

        //Getting I-Node table from disk
        read_blocks(1, 6, i_node_table); 

        //Getting Directory Table from disk
        read_blocks(7, 2, directory_table); 

        //Getting Free Bit Map from disk
        read_blocks(1023, 1, free_bit_map);
    }
}

int sfs_fopen(char *name){

    int existing_file_found = 0; 
    int existing_i_node_number = -1; 
    int file_descriptor_index = -1; 

    //Checking if the given file name is too long (exceeds 15 characters + '\0')
    if (strlen(name) > 15){
        return -1; 
    }

    //Checking whether the file already exsists on the system (exists inside of the Directory Table)
    for (int i = 0; i < 96; i++){
        if (directory_table[i].entry_used == '1' && strcmp(directory_table[i].filename, name) == 0){
            existing_i_node_number = directory_table[i].i_node_number; 
            existing_file_found = 1; 
            break; 
        }
    }

    //Case 1: File already exists, need to check if it's open or not
    if (existing_file_found == 1){

        //Case 1.A: File exist, need to check if it's open (can be found in the FDT) 
        for (int i = 0; i < 10; i++){

            //File was found in the FDT
            if (file_descriptor_table[i].i_node_number == existing_i_node_number){
                return i; 
            }
        }

        //Case 1.B: File exists, but it is not open, need to add it to the FDT
        for (int i = 0; i < 10; i++){
            if (file_descriptor_table[i].i_node_number == -1){
                file_descriptor_table[i].i_node_number = existing_i_node_number; 
                file_descriptor_table[i].read_write_pointer = i_node_table[existing_i_node_number].file_size;
                return i; 
            }
        }
    }

    //Case 2: File does not exist, need to create it from scratch
    else{
        //=================================================I-NODE===================================================

        //Allocate and initialize an I-Node 
        struct i_node *temp_i_node = (struct i_node*)malloc(sizeof(struct i_node));

        //Set I-Node file size to 0 
        temp_i_node->file_size = 0; 

        //Setting the pointers (direct and indirect) to -1 to show that they're unused 
        for(int i = 0; i<12; i++){
            temp_i_node->direct_pointer[i] = -1; 
        }
        temp_i_node->indirect_pointer = -1; 

        //Find empty slot for the new node inside of the i_node_table
        int index_of_i_node = -1; 
        for (int i = 0; i < 114; i++){ 

            //Found a slot on the i_node_table
            if (i_node_table[i].file_size == -1){
                i_node_table[i] = *temp_i_node; //Storing the new i-node inside the table
                index_of_i_node = i; 
                break; 
            }
        }

        //Updating the i_node_table on the disk
        write_blocks(1, 6, i_node_table);

        //============================================DIRECTORY=====================================================

        //Create a new Directory Entry for the directory_table
        struct directory_entry *temp_directory = (struct directory_entry*)malloc(sizeof(struct directory_entry)); 
        temp_directory->entry_used = '1'; 
        strcpy(temp_directory->filename, name); 
        temp_directory->i_node_number = index_of_i_node; 

        //Find empty slot for the new directory inside of the directory_table 
        for (int i = 0; i < 96; i++){

            //Found a slot in the directory_table
            if (directory_table[i].entry_used == '0'){
                directory_table[i] = *temp_directory; 
                break; 
            }
        }

        //Updating the directory_table on the disk
        write_blocks(7, 2, directory_table); 

        //=======================================FILE DESCRIPTOR TABLE==============================================

        //Create a File Descriptor Entry for the file_descriptor_table
        struct file_descriptor_entry *temp_descriptor_entry = (struct file_descriptor_entry*)malloc(sizeof(struct file_descriptor_entry)); 
        temp_descriptor_entry->i_node_number = index_of_i_node; 
        temp_descriptor_entry->read_write_pointer = 0; //new file, so pointer is at the start of the file 

        //Find empty slot for the new file descriptor entry in the file_descriptor_table
        for (int i = 0; i < 10; i++){
            
            //Found a slot in the file_descriptor_table
            if (file_descriptor_table[i].i_node_number == -1){
                file_descriptor_table[i] = *temp_descriptor_entry;
                file_descriptor_index = i; 
                break; 
            }
        }
    } 
    return file_descriptor_index; 
}

int sfs_fclose(int fileID){

    //Case where we're trying to close a file that is not open in the first place
    if (file_descriptor_table[fileID].i_node_number == -1){
        return -1; 
    }

    //Case where the file is open, and we now close it
    else{
        file_descriptor_table[fileID].i_node_number = -1; //-1 signifies that the slot is not in use! 
        return 0; 
    }
}
 
int sfs_fwrite(int fileID, const char *buf, int length){
    //Getting the amount of bytes left to write initially
    int bytes_left_to_write = length; 

    //Getting the i_node_number using fileID from the FDT
    int i_node = file_descriptor_table[fileID].i_node_number;
    
    //Checking if the file we are trying to write to is open
    if (i_node == -1){
        return 0; 
    }

    //Getting the current file size 
    int current_i_node_size = i_node_table[i_node].file_size; 
    int i_node_file_size_before = i_node_table[i_node].file_size; 

    //Checking if writing 'length' bytes to this file will exceed the established max file size
    if (i_node_table[i_node].file_size + length >= 274432){

        //Decreasing the number of bytes to write to avoid exceeding the limit
        bytes_left_to_write = 274431 - i_node_table[i_node].file_size;
    }

    //Identifying the block that will be used to write at each iteration (0-11 means direct pointer blocks and 12-268 means indirect pointer blocks)
    int current_block_pointer = file_descriptor_table[fileID].read_write_pointer / 1024; 

    //Creating a pointer to keep track of how much of the "buf" array has been written to the disk, initially nothing is written, so = 0
    int temp_write_pointer = 0; 

    //Case where the read_write_pointer of the file points to a location that is within the direct pointer blocks (between 0 and 11)
    if (current_block_pointer <= 11){

        //Starting from the current direct pointer, filling up the direct pointer blocks until we exceed the direct block limit (0 to 12288)
        for (int i = current_block_pointer; i < 12; i++){

            //Calculating the number of free bytes that can be written to the current direct block i 
            int remaining_bytes_in_block = (1024 * (i + 1)) - current_i_node_size; 
            int bytes_already_in_block = remaining_bytes_in_block;

            //Special case where no bytes can be written at all 
            if (remaining_bytes_in_block == 0){
                continue; 
            }
            else{
                //The i_node table never allocated this direct_pointer, must find slot in the FBM
                if (i_node_table[i_node].direct_pointer[i] == -1){

                    //Search bitmap to find next empty block and set the current direct_pointer to that block
                    for (int j = 0; j < 1024; j++){
                        if (free_bit_map[j] == '1'){
                            free_bit_map[j] = '0'; 
                            i_node_table[i_node].direct_pointer[i] = j;
                            break;  
                        }
                    }
                }

                //Initializing char array to hold the content from the disk block
                char block_data[1024];

                //Reading the data of the current block from the disk
                read_blocks(i_node_table[i_node].direct_pointer[i], 1, (void *)block_data); 

                //Case where the data coming in doesn't completely fill up the block 
                if (remaining_bytes_in_block >= bytes_left_to_write){
                    remaining_bytes_in_block = bytes_left_to_write; 
                }

                //Updating the size of the i-node 
                current_i_node_size = current_i_node_size + remaining_bytes_in_block; 

                //Copying a part of the data in buf into the block
                //(1024-bytes_already_in_block) ensures that we don't overwrite data that was previously written
                memcpy(block_data + (1024-bytes_already_in_block), buf + temp_write_pointer, remaining_bytes_in_block); 

                //Updating how much of the given data was written. 
                temp_write_pointer = temp_write_pointer + remaining_bytes_in_block;

                //Write the new block back into the disk
                write_blocks(i_node_table[i_node].direct_pointer[i], 1, (void *)block_data);

                //Updating the number of bytes left to write
                bytes_left_to_write = bytes_left_to_write - remaining_bytes_in_block; 

                //All the data from buf has been copied into the disk
                if (bytes_left_to_write == 0){
                    i_node_table[i_node].file_size = current_i_node_size;
                    current_block_pointer = i; 
                    break; 
                }
            }     

            //Needed for the indirect pointer indexation 
            current_block_pointer = i; 
        }
    }

    //Case where we need to use the indirect pointer blocks (either read_write_pointer starts here, or direct blocks were not sufficient)
    if (temp_write_pointer < length){

        //Fixing the current_pointer_block if direct pointer blocks were previously written to
        if (current_block_pointer == 11){
            current_block_pointer = 12; 
        }

        //Creating an array for the indirect pointer block with block numbers 
        uint32_t indirect_block[1024];

        // Case where the indirect pointer hasn't been used before, need to find free block in FBM
        if (i_node_table[i_node].indirect_pointer == -1){

            //Search bitmap to find next empty block and set the pointer to that
            for (int j = 0; j < 1024; j++){
                if (free_bit_map[j] == '1'){
                    free_bit_map[j] = '0'; 
                    i_node_table[i_node].indirect_pointer = j;
                    break;  
                }
            }
        }

        // Case where the indirect pointer has been used before, need to fetch it from memory
        else{
            read_blocks(i_node_table[i_node].indirect_pointer, 1, (void *)indirect_block); 
        }
        
        //Will hold specific block numbers stored inside the indirect pointer block
        int block_index = -1;
        //Will hold the content of each specific block number stored inside the indirect pointer block
        char indirect_block_data[1024]; 

        //Keep iterating while there are still bytes to be written
        while(bytes_left_to_write != 0){
            
            //Calculating the number of free bytes that can be written to the current indirect block
            int remaining_bytes_in_block = (1024 * (current_block_pointer + 1)) - current_i_node_size;
            int bytes_already_in_block = remaining_bytes_in_block;

            //This indirect block has not been written to before, need to find it in the FBM 
            if (current_i_node_size <= (1024 * current_block_pointer)){
                for (int j = 0; j < 1024; j++){
                    if (free_bit_map[j] == '1'){
                        free_bit_map[j] = '0'; 
                        indirect_block[current_block_pointer-12] = j; 
                        block_index = j;
                        break;  
                    }
                }
            }

            //This indirect block has been written to before, need to fetch it from the disk
            else{
                block_index = indirect_block[current_block_pointer-12];
                read_blocks(block_index, 1, (void *)indirect_block_data);
            }

            //Case where the data coming in doesn't completely fill up the block 
            if (remaining_bytes_in_block >= bytes_left_to_write){
                remaining_bytes_in_block = bytes_left_to_write; 
            }

            //Updating the size of the i-node 
            current_i_node_size = current_i_node_size + remaining_bytes_in_block; 

            //Copying a part of the data in buf into the block
            //(1024-bytes_already_in_block) ensures that we don't overwrite data that was previously written
            memcpy(indirect_block_data + (1024 - bytes_already_in_block), buf + temp_write_pointer, remaining_bytes_in_block); 

            //Updating how much of the given data was written. 
            temp_write_pointer = temp_write_pointer + remaining_bytes_in_block;

            //Write the new block back into the disk
            write_blocks(block_index, 1, (void *)indirect_block_data);
            //Write the indirect pointer block back into the disk
            write_blocks(i_node_table[i_node].indirect_pointer, 1, (void *)indirect_block); 

            //Updating the number of bytes left to write
            bytes_left_to_write = bytes_left_to_write - remaining_bytes_in_block; 
            
            //Going to the next block in the indirect pointer 
            current_block_pointer = current_block_pointer + 1; 

            //All the data from buf has been copied into the disk
            if (bytes_left_to_write == 0){
                i_node_table[i_node].file_size = current_i_node_size;
                break; 
            }
        }
    }

    //Updating the i_node_table on the disk  
    write_blocks(1, 6, i_node_table); 
    //Updating the free_bit_map on the disk
    write_blocks(1023, 1, free_bit_map); 

    //Moving the read_write_pointer in FDT to its prev_value + bytes written
    file_descriptor_table[fileID].read_write_pointer = file_descriptor_table[fileID].read_write_pointer + (i_node_table[i_node].file_size - i_node_file_size_before);
    
    return (i_node_table[i_node].file_size - i_node_file_size_before);
}


int sfs_fread(int fileID, char *buf, int length){
    int total_bytes_read = 0; 

    //Need to get the read/write pointer from FDT
    int read_write_pointer = file_descriptor_table[fileID].read_write_pointer;

    //Get the i-node using fileID from the file_descriptor_table 
    int i_node = file_descriptor_table[fileID].i_node_number;

    //Checking if the file we are trying to read from is open
    if (i_node == -1){
        return -1; 
    }

    //Getting the current size of the i-node
    int i_node_size = i_node_table[i_node].file_size; 

    //Calculating which block the read_write_pointer is pointing to
    int pointed_block = read_write_pointer / 1024; 
    
    //Used to adjust the reading process if it's not started from the beginning of a block (updated for each block)
    int read_write_offset = read_write_pointer - (pointed_block * 1024); 

    //Getting the number of blocks to read
    int number_of_block_to_read = length / 1024; 

    int block_index = -1; 
    uint32_t block_indices[1024];

    //Case where indirect blocks are needed, fetch indirect pointer block from disk
    if (number_of_block_to_read >= 12){
        block_index = i_node_table[i_node].indirect_pointer;
        read_blocks(block_index, 1, (void *)block_indices); 
    }

    int remaining_bytes_to_read = length; 

    //Case where it's trying to read more bytes than the i-node contains - adjusting it
    if (i_node_size < length){
        remaining_bytes_to_read = i_node_size; 
    }

    //Keep reading while there's something to read
    while (remaining_bytes_to_read > 0){
        char block_data[1024];

        //Case where the read_write_pointer is somewhere between 0 and 12288 (within the bounds of direct pointers)
        if (pointed_block <= 11){
            //Getting the direct block from the disk
            block_index = i_node_table[i_node].direct_pointer[pointed_block];
            read_blocks(block_index, 1, (void *)block_data); 

            //Case where the entire content of the block can be read (copied) into buf
            if (remaining_bytes_to_read >= 1024){
                //Copying the data into buf 
                memcpy(buf + total_bytes_read, block_data + read_write_offset, 1024 - read_write_offset); 
                remaining_bytes_to_read = remaining_bytes_to_read - (1024 - read_write_offset); 
                total_bytes_read = total_bytes_read + (1024 - read_write_offset); 
                read_write_offset = 0; 
            }

            //Case where we have less than 1024 bytes left to read
            else{
                
                //Case where the data cannot be copied fully into the block! will need another read
                if (read_write_offset + remaining_bytes_to_read > 1024){
                    memcpy(buf + total_bytes_read, block_data + read_write_offset, (1024 - read_write_offset));  
                    remaining_bytes_to_read = remaining_bytes_to_read - (1024 - read_write_offset); 
                    total_bytes_read = total_bytes_read + (1024 - read_write_offset);
                }

                //Case where the data can be fully copied into the block
                else{
                    memcpy(buf + total_bytes_read, block_data + read_write_offset, remaining_bytes_to_read); 
                    total_bytes_read = total_bytes_read + remaining_bytes_to_read;
                    remaining_bytes_to_read = 0; 
                }
                
                //Resetting the offset for the next block
                read_write_offset = 0; 
            }
        }

        //Case where we need to access the indirect blocks (either read_write_pointer points there or need to read beyond direct blocks)
        else{
            block_index = block_indices[pointed_block-12];
            read_blocks(block_index, 1, (void *)block_data);

            //Case where the entire content of the block can be read (copied) into buf
            if (remaining_bytes_to_read >= 1024){
                memcpy(buf + total_bytes_read, block_data + read_write_offset, 1024 - read_write_offset); 
                remaining_bytes_to_read = remaining_bytes_to_read - (1024 - read_write_offset);
                total_bytes_read = total_bytes_read + (1024 - read_write_offset);
                read_write_offset = 0;
                
            }

            //Case where we have less than 1024 bytes left to read
            else{

                //Case where the data cannot be copied fully into the block! will need another read
                if (read_write_offset + remaining_bytes_to_read > 1024){
                    memcpy(buf + total_bytes_read, block_data + read_write_offset, (1024 - read_write_offset));  //CHANGE, WATCHOUT**********************************
                    remaining_bytes_to_read = remaining_bytes_to_read - (1024 - read_write_offset); 
                    total_bytes_read = total_bytes_read + (1024 - read_write_offset);
                }

                //Case where the data can be fully copied into the block
                else{
                    memcpy(buf + total_bytes_read, block_data + read_write_offset, remaining_bytes_to_read);  //CHANGE, WATCHOUT**********************************
                    total_bytes_read = total_bytes_read + remaining_bytes_to_read;
                    remaining_bytes_to_read = 0; 
                }

                //Resetting the offset for the next block
                read_write_offset = 0;
            }
        }

        pointed_block = pointed_block + 1; 
    }

    //Updating the read_write_pointer
    file_descriptor_table[fileID].read_write_pointer = file_descriptor_table[fileID].read_write_pointer + total_bytes_read; 

    return total_bytes_read;
}

int sfs_fseek(int fileID, int loc){

    //Checking whether the current fileID points to an i-node that is use (AKA file exists) 
    if (file_descriptor_table[fileID].i_node_number == -1){
        return -1; 
    }
    //Case where the i-node is valid and exists
    else{
        file_descriptor_table[fileID].read_write_pointer = loc; 
        return 0; 
    }

}

int sfs_getfilesize(const char* path){
    int filesize = -1; 

    //Looking for the file in the Directory Table
    for (int i = 0; i < 96; i++){
        if (directory_table[i].entry_used == '1' && strcmp(directory_table[i].filename, path) == 0){
            filesize = i_node_table[directory_table[i].i_node_number].file_size;
            break; 
        }
    }

    return filesize;
}

int sfs_getnextfilename(char *fname){

    //Looking for the next file in the Directory Table and updating the `current_file_read` pointer
    for (int i = (current_file_read + 1); i < 96; i++){
        if (directory_table[i].entry_used == '1'){
            current_file_read = i; 
            strcpy(fname, directory_table[i].filename); 
            return 1; 
        }
    }
    
    return 0; 
}

int sfs_remove(char *file){
    int i_node = -1; 
    int node_filesize = -1; 

    //Get the inode from the directory table and set it as unused
    for (int i = 0; i < 96; i++){
        if (strcmp(directory_table[i].filename, file) == 0){
            i_node = directory_table[i].i_node_number;
            directory_table[i].entry_used = '0'; // 0 == free | 1 == used
            break; 
        }
    }

    //If the file doesn't exist, we cannot remove it from the system
    if (i_node == -1){
        return -1; 
    }

    //Update the Directory Table on the disk 
    write_blocks(7, 2, directory_table); 

    //If the file was open, close (remove from FDT) 
    for (int i = 0; i < 10; i++){
        if (file_descriptor_table[i].i_node_number == i_node){
            file_descriptor_table[i].i_node_number = -1; 
            break; 
        }
    }

    node_filesize = i_node_table[i_node].file_size;

    //Setting the file_size as empty to indicate that th i-node is no longer in use
    i_node_table[i_node].file_size = -1; 
    
    //Getting the number of blocks used for this file
    int number_of_blocks = node_filesize / 1024; 

    int block_index = 0; 

    //Cas ewhere the file was not empty
    if (node_filesize != 0){
        //Free the blocks used for the direct pointers 
        while (1){
            
            //Freeing the block used for the direct pointer
            free_bit_map[i_node_table[i_node].direct_pointer[block_index]] = '1'; 

            //Case where we finished clearing all the used indirect blocks -> exit loop
            if (block_index == number_of_blocks || block_index == 11){
                break; 
            }

            block_index++; 
        }

        //Case where there are more than 12 blocks, then the indirect pointer is involved
        if (number_of_blocks >= 12) {
            block_index = 12; 
            uint32_t indirect_block[1024];

            //Getting the indirect block from the disk
            read_blocks(i_node_table[i_node].indirect_pointer, 1, (void *)indirect_block);

            //Clearing the slot in the FBM for the indirect index block 
            free_bit_map[i_node_table[i_node].indirect_pointer] = '1'; 

            //Going over the block numbers shown in the indirect pointer and "freeing" them in the FBM
            while (block_index <= number_of_blocks){
                free_bit_map[indirect_block[block_index-12]] = '1'; 
                block_index++; 
            }
        }
        //Update the FBM  on the disk 
        write_blocks(1023, 1, free_bit_map);
    }

    //Update the I-Node Table on the disk 
    write_blocks(1, 6, i_node_table); 

    return 0; 
}