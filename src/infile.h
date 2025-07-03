/**\file    infile.h
 * \ingroup Samplers
 */
#pragma once

bool infile_set (const char *arg);
bool informat_set (const char *arg);
bool infile_init (void);
int  infile_read (void);
void infile_exit (void);



