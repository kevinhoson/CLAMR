/*
 *  Copyright (c) 2014, Los Alamos National Security, LLC.
 *  All rights Reserved.
 *
 *  Copyright 2011-2012. Los Alamos National Security, LLC. This software was produced 
 *  under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National 
 *  Laboratory (LANL), which is operated by Los Alamos National Security, LLC 
 *  for the U.S. Department of Energy. The U.S. Government has rights to use, 
 *  reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS 
 *  ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR 
 *  ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is modified
 *  to produce derivative works, such modified software should be clearly marked,
 *  so as not to confuse it with the version available from LANL.
 *
 *  Additionally, redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Los Alamos National Security, LLC, Los Alamos 
 *       National Laboratory, LANL, the U.S. Government, nor the names of its 
 *       contributors may be used to endorse or promote products derived from 
 *       this software without specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE LOS ALAMOS NATIONAL SECURITY, LLC AND 
 *  CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT 
 *  NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL
 *  SECURITY, LLC OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 *  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *  
 *  CLAMR -- LA-CC-11-094
 *  
 *  Authors: Brian Atkinson          bwa@g.clemson.edu
             Bob Robey        XCP-2  brobey@lanl.gov
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <assert.h>
#include "PowerParser/PowerParser.hh"

#include "crux.h"
#include "timer/timer.h"
#include "fmemopen.h"

#ifdef HAVE_HDF5
#include "hdf5.h"
#endif
#ifdef HAVE_MPI
#include "mpi.h"
#endif

const bool CRUX_TIMING = true;
bool do_crux_timing = false;

bool h5_spoutput;

#define RESTORE_NONE     0
#define RESTORE_RESTART  1
#define RESTORE_ROLLBACK 2

#ifndef DEBUG
#define DEBUG 0
#endif

using namespace std;
using PP::PowerParser;
// Pointers to the various objects.
PowerParser *parse;

char checkpoint_directory[] = "checkpoint_output";
int cp_num, rs_num;
int *backup;
void **crux_data;
size_t *crux_data_size;
#ifdef HAVE_HDF5
bool USE_HDF5 = true; //MSB
hid_t h5_fid;
hid_t h5_gid_c, h5_gid_m, h5_gid_s;
herr_t h5err;
#endif

FILE *crux_time_fp;
struct timeval tcheckpoint_time;
struct timeval trestore_time;
int checkpoint_timing_count = 0;
float checkpoint_timing_sum = 0.0f;
float checkpoint_timing_size = 0.0f;
int rollback_attempt = 0;
FILE *store_fp, *restore_fp;
#ifdef HAVE_MPI
static MPI_File mpi_store_fp, mpi_restore_fp;
#endif
static int mype = 0;

Crux::Crux(int crux_type_in, int num_of_rollback_states_in, bool restart)
{
#ifdef HAVE_MPI
   MPI_Comm_rank(MPI_COMM_WORLD,&mype);
#endif

   num_of_rollback_states = num_of_rollback_states_in;
   crux_type = crux_type_in;
   checkpoint_counter = 0;

   if (crux_type != CRUX_NONE || restart){
      do_crux_timing = CRUX_TIMING;
      struct stat stat_descriptor;
      if (stat(checkpoint_directory,&stat_descriptor) == -1){
        mkdir(checkpoint_directory,0777);
      }
   }

   crux_data = (void **)malloc(num_of_rollback_states*sizeof(void *));
   for (int i = 0; i < num_of_rollback_states; i++){
      crux_data[i] = NULL;
   }
   crux_data_size = (size_t *)malloc(num_of_rollback_states*sizeof(size_t));


   if (do_crux_timing){
      char checkpointtimelog[60];
      sprintf(checkpointtimelog,"%s/crux_timing.log",checkpoint_directory);
      crux_time_fp = fopen(checkpointtimelog,"w");
   }
}

Crux::~Crux()
{
   for (int i = 0; i < num_of_rollback_states; i++){
      free(crux_data[i]);
   }
   free(crux_data);
   free(crux_data_size);

   if (do_crux_timing){
      if (checkpoint_timing_count > 0) {
         printf("CRUX checkpointing time averaged %f msec, bandwidth %f Mbytes/sec\n",
                checkpoint_timing_sum/(float)checkpoint_timing_count*1.0e3,
                checkpoint_timing_size/checkpoint_timing_sum*1.0e-6);

         fprintf(crux_time_fp,"CRUX checkpointing time averaged %f msec, bandwidth %f Mbytes/sec\n",
                checkpoint_timing_sum/(float)checkpoint_timing_count*1.0e3,
                checkpoint_timing_size/checkpoint_timing_sum*1.0e-6);

      fclose(crux_time_fp);
      }
   }
}

void Crux::store_MallocPlus(MallocPlus memory){
}

void Crux::store_begin(size_t nsize, int ncycle)
{
   cp_num = checkpoint_counter % num_of_rollback_states;

   cpu_timer_start(&tcheckpoint_time);

   if(crux_type == CRUX_IN_MEMORY){
      if (crux_data[cp_num] != NULL) free(crux_data[cp_num]);
      crux_data[cp_num] = (int *)malloc(nsize);
      crux_data_size[cp_num] = nsize;
      store_fp = fmemopen(crux_data[cp_num], nsize, "w");
   }
   if(crux_type == CRUX_DISK){
      char backup_file[60];

#ifdef HAVE_HDF5
      hid_t plist_id;

      if(USE_HDF5) {
        sprintf(backup_file,"%s/backup%05d.h5",checkpoint_directory,ncycle);

        
        plist_id = H5P_DEFAULT; 
#  ifdef HAVE_MPI
        int mpiInitialized = 0;
        bool phdf5 = false;
        if (MPI_SUCCESS == MPI_Initialized(&mpiInitialized)) {
          phdf5 = true;
        }

        // 
        // Set up file access property list with parallel I/O access
        //
        if( (plist_id = H5Pcreate(H5P_FILE_ACCESS)) < 0)
          printf("HDF5: Could not create property list \n");

	if( H5Pset_libver_bounds(plist_id, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST) < 0)
          printf("HDF5: Could set libver bounds \n");

        H5Pset_fapl_mpio(plist_id, MPI_COMM_WORLD, MPI_INFO_NULL);
#  endif
        h5_fid = H5Fcreate(backup_file, H5F_ACC_TRUNC, H5P_DEFAULT, plist_id);
        if(!h5_fid){
          printf("HDF5: Could not write HDF5 %s at iteration %d\n",backup_file,ncycle);
        }
        if( (h5_gid_c = H5Gcreate(h5_fid, "clamr", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) ) < 0) 
          printf("HDF5: Could not create \"clamr\" group \n");
        if( (h5_gid_m = H5Gcreate(h5_fid, "mesh", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) ) < 0)
          printf("HDF5: Could not create \"mesh\" group \n");
        if( (h5_gid_s = H5Gcreate(h5_fid, "state", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) ) < 0)
          printf("HDF5: Could not create \"state\" group \n");
      }

#  ifdef HAVE_MPI
      if(H5Pclose(plist_id) < 0)
        printf("HDF5: Could not close property list \n");
#  endif

#endif
      sprintf(backup_file,"%s/backup%05d.crx",checkpoint_directory,ncycle);
#ifdef HAVE_MPI
      int iret = MPI_File_open(MPI_COMM_WORLD, backup_file, MPI_MODE_CREATE|MPI_MODE_WRONLY, MPI_INFO_NULL, &mpi_store_fp);
      if(iret != MPI_SUCCESS) {
         printf("Could not write %s at iteration %d\n",backup_file,ncycle);
      }
#else
      store_fp = fopen(backup_file,"w");
      if(!store_fp){
         printf("Could not write %s at iteration %d\n",backup_file,ncycle);
      }
#endif

      if (mype == 0) {
        char symlink_file[60];
        sprintf(symlink_file,"%s/backup%1d.crx",checkpoint_directory,cp_num);
        symlink(backup_file, symlink_file);
//      int ireturn = symlink(backup_file, symlink_file);
//      if (ireturn == -1) {
//         printf("Warning: error returned with symlink call for file %s and symlink %s\n",
//                backup_file,symlink_file);
//      }
      }
   }

   if (do_crux_timing){
      checkpoint_timing_size += nsize;
   }
}

void Crux::store_field_header(const char *name, int name_size){
#ifdef HAVE_MPI
   assert(name != NULL);
   MPI_Status status;
   MPI_File_write_shared(mpi_store_fp, (void *)name, name_size, MPI_CHAR, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_CHAR, &count);
   printf("Wrote %d characters at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   assert(name != NULL && store_fp != NULL);
   fwrite(name,sizeof(char),name_size,store_fp);
#endif
}

void Crux::store_bools(bool *bool_vals, size_t nelem)
{
   assert(bool_vals != NULL && store_fp != NULL);
   fwrite(bool_vals,sizeof(bool),nelem,store_fp);
}

void Crux::store_ints(int *int_vals, size_t nelem)
{
   assert(int_vals != NULL && store_fp != NULL);
   fwrite(int_vals,sizeof(int),nelem,store_fp);
}

void Crux::store_longs(long long *long_vals, size_t nelem)
{
   assert(long_vals != NULL && store_fp != NULL);
   fwrite(long_vals,sizeof(long long),nelem,store_fp);
}

void Crux::store_sizets(size_t *size_t_vals, size_t nelem)
{
   assert(size_t_vals != NULL && store_fp != NULL);
   fwrite(size_t_vals,sizeof(size_t),nelem,store_fp);
}

void Crux::store_doubles(double *double_vals, size_t nelem)
{
   assert(double_vals != NULL && store_fp != NULL);
   fwrite(double_vals,sizeof(double),nelem,store_fp);
}

void Crux::store_int_array(int *int_array, size_t nelem)
{
#ifdef HAVE_MPI
   assert(int_array != NULL);
   MPI_Status status;
   MPI_File_write_shared(mpi_store_fp, int_array, (int)nelem, MPI_INT, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_INT, &count);
   printf("Wrote %d integers at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   assert(int_array != NULL && store_fp != NULL);
   fwrite(int_array,sizeof(int),nelem,store_fp);
#endif
}

void Crux::store_long_array(long long *long_array, size_t nelem)
{
   assert(long_array != NULL && store_fp != NULL);
   fwrite(long_array,sizeof(long long),nelem,store_fp);
}

void Crux::store_float_array(float *float_array, size_t nelem)
{
   assert(float_array != NULL && store_fp != NULL);
   fwrite(float_array,sizeof(float),nelem,store_fp);
}

void Crux::store_double_array(double *double_array, size_t nelem)
{
#ifdef HAVE_MPI
   assert(double_array != NULL);
   MPI_Status status;
   MPI_File_write_shared(mpi_store_fp, double_array, (int)nelem, MPI_DOUBLE, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_DOUBLE, &count);
   printf("Wrote %d doubles at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   assert(double_array != NULL && store_fp != NULL);
   fwrite(double_array,sizeof(double),nelem,store_fp);
#endif
}

void Crux::store_replicated_int_array(int *int_array, size_t nelem)
{
#ifdef HAVE_MPI
   assert(int_array != NULL);
   MPI_Status status;
   MPI_File_write_shared(mpi_store_fp, int_array, (int)nelem, MPI_INT, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_INT, &count);
   printf("Wrote %d integers at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   assert(int_array != NULL && store_fp != NULL);
   fwrite(int_array,sizeof(int),nelem,store_fp);
#endif
}

void Crux::store_replicated_double_array(double *double_array, size_t nelem)
{
#ifdef HAVE_MPI
   assert(double_array != NULL);
   MPI_Status status;
   MPI_File_write_shared(mpi_store_fp, double_array, (int)nelem, MPI_DOUBLE, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_DOUBLE, &count);
   printf("Wrote %d doubles at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   assert(double_array != NULL && store_fp != NULL);
   fwrite(double_array,sizeof(double),nelem,store_fp);
#endif
}

void Crux::store_end(void)
{
#ifdef HAVE_MPI
   MPI_File_close(&mpi_store_fp);
#else
   assert(store_fp != NULL);
   fclose(store_fp);
#endif

#ifdef HAVE_HDF5
   if(USE_HDF5) {
     if(H5Gclose(h5_gid_c) < 0)
       printf("HDF5: Could not close clamr group \n");
     if(H5Gclose(h5_gid_m) < 0)
       printf("HDF5: Could not close mesh group \n");
     if(H5Gclose(h5_gid_s) < 0)
       printf("HDF5: Could not close state group \n");
     if(H5Fclose(h5_fid) != 0) {
       printf("HDF5: Could not close HDF5 file \n");
     }
   }
#endif

   double checkpoint_total_time = cpu_timer_stop(tcheckpoint_time);

   if (do_crux_timing){
      fprintf(crux_time_fp, "Total time for checkpointing was %g seconds\n", checkpoint_total_time);
      checkpoint_timing_count++;
      checkpoint_timing_sum += checkpoint_total_time;
   }

   checkpoint_counter++;
}

int restore_type = RESTORE_NONE;

void Crux::restore_MallocPlus(MallocPlus memory){
}

void Crux::restore_begin(char *restart_file, int rollback_counter)
{
   rs_num = rollback_counter % num_of_rollback_states;

   cpu_timer_start(&trestore_time);

   if (restart_file != NULL){
      if (mype == 0) {
         printf("\n  ================================================================\n");
         printf(  "  Restoring state from disk file %s\n",restart_file);
         printf(  "  ================================================================\n\n");
      }
#ifdef HAVE_MPI
      int iret = MPI_File_open(MPI_COMM_WORLD, restart_file, MPI_MODE_RDONLY | MPI_MODE_UNIQUE_OPEN, MPI_INFO_NULL, &mpi_restore_fp);
      if(iret != MPI_SUCCESS){
         //printf("Could not write %s at iteration %d\n",restart_file,crux_int_vals[8]);
         printf("Could not open restart file %s\n",restart_file);
      }

#else
      restore_fp = fopen(restart_file,"r");
      if(!restore_fp){
         //printf("Could not write %s at iteration %d\n",restart_file,crux_int_vals[8]);
         printf("Could not open restart file %s\n",restart_file);
      }
#endif
      restore_type = RESTORE_RESTART;
   } else if(crux_type == CRUX_IN_MEMORY){
      printf("Restoring state from memory rollback number %d rollback_counter %d\n",rs_num,rollback_counter);
      restore_fp = fmemopen(crux_data[rs_num], crux_data_size[rs_num], "r");
      restore_type = RESTORE_ROLLBACK;
   } else if(crux_type == CRUX_DISK){
      char backup_file[60];

      sprintf(backup_file,"%s/backup%d.crx",checkpoint_directory,rs_num);
      printf("Restoring state from disk file %s rollback_counter %d\n",backup_file,rollback_counter);
      restore_fp = fopen(backup_file,"r");
      if(!restore_fp){
         //printf("Could not write %s at iteration %d\n",backup_file,crux_int_vals[8]);
         printf("Could not open restore file %s\n",backup_file);
      }
      restore_type = RESTORE_ROLLBACK;
   }
}

void Crux::restore_field_header(char *name, int name_size)
{
#ifdef HAVE_MPI
   assert(name != NULL);
   MPI_Status status;
   MPI_File_read_shared(mpi_restore_fp, name, name_size, MPI_CHAR, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_CHAR, &count);
   printf("Read %d characters at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   int name_read = fread(name,sizeof(char),name_size,restore_fp);
   if (name_read != name_size){
      printf("Warning: number of elements read %d is not equal to request %d\n",name_read,name_size);
   }
#endif
}

void Crux::restore_bools(bool *bool_vals, size_t nelem)
{
   size_t nelem_read = fread(bool_vals,sizeof(bool),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
}

void Crux::restore_ints(int *int_vals, size_t nelem)
{
   size_t nelem_read = fread(int_vals,sizeof(int),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
}

void Crux::restore_longs(long long *long_vals, size_t nelem)
{
   size_t nelem_read = fread(long_vals,sizeof(long),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
}

void Crux::restore_sizets(size_t *size_t_vals, size_t nelem)
{
   size_t nelem_read = fread(size_t_vals,sizeof(size_t),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
}

void Crux::restore_doubles(double *double_vals, size_t nelem)
{
   size_t nelem_read = fread(double_vals,sizeof(double),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
}

int *Crux::restore_int_array(int *int_array, size_t nelem)
{
#ifdef HAVE_MPI
   assert(int_array != NULL);
   MPI_Status status;
   MPI_File_read_shared(mpi_restore_fp, int_array, (int)nelem, MPI_INT, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_INT, &count);
   printf("Read %d integers at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   size_t nelem_read = fread(int_array,sizeof(int),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
#endif
   return(int_array);
}

long long *Crux::restore_long_array(long long *long_array, size_t nelem)
{
   size_t nelem_read = fread(long_array,sizeof(long long),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
   return(long_array);
}

float *Crux::restore_float_array(float *float_array, size_t nelem)
{
   size_t nelem_read = fread(float_array,sizeof(float),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
   return(float_array);
}

double *Crux::restore_double_array(double *double_array, size_t nelem)
{
#ifdef HAVE_MPI
   MPI_Status status;
   MPI_File_read_shared(mpi_restore_fp, double_array, (int)nelem, MPI_DOUBLE, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_DOUBLE, &count);
   printf("Read %d doubles at line %d in file %s\n",count,__LINE__,__FILE__);
#endif
  
#else
   size_t nelem_read = fread(double_array,sizeof(double),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
#endif
   return(double_array);
}

int *Crux::restore_replicated_int_array(int *int_array, size_t nelem)
{
#ifdef HAVE_MPI
   assert(int_array != NULL);
   MPI_Status status;
   MPI_File_read_shared(mpi_restore_fp, int_array, (int)nelem, MPI_INT, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_INT, &count);
   printf("Read %d integers at line %d in file %s\n",count,__LINE__,__FILE__);
#endif

#else
   size_t nelem_read = fread(int_array,sizeof(int),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
#endif
   return(int_array);
}

double *Crux::restore_replicated_double_array(double *double_array, size_t nelem)
{
#ifdef HAVE_MPI
   MPI_Status status;
   MPI_File_read_shared(mpi_restore_fp, double_array, (int)nelem, MPI_DOUBLE, &status);
#ifdef DEBUG_RESTORE_VALS
   int count;
   MPI_Get_count(&status, MPI_DOUBLE, &count);
   printf("Read %d doubles at line %d in file %s\n",count,__LINE__,__FILE__);
#endif
  
#else
   size_t nelem_read = fread(double_array,sizeof(double),nelem,restore_fp);
   if (nelem_read != nelem){
      printf("Warning: number of elements read %lu is not equal to request %lu\n",nelem_read,nelem);
   }
#endif
   return(double_array);
}

void Crux::restore_end(void)
{
   double restore_total_time = cpu_timer_stop(trestore_time);

   if (do_crux_timing){
      if (restore_type == RESTORE_RESTART) {
         fprintf(crux_time_fp, "Total time for restore was %g seconds\n", restore_total_time);
      } else if (restore_type == RESTORE_ROLLBACK){
         fprintf(crux_time_fp, "Total time for rollback %d was %g seconds\n", rollback_attempt, restore_total_time);
      }
   }

   fclose(restore_fp);

}

int Crux::get_rollback_number()
{
  rollback_attempt++;
  return(checkpoint_counter % num_of_rollback_states);
}
