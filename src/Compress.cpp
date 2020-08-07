#include <stdlib.h>
#include <Python.h>
#include "bytesobject.h"
#include "zstd.h"
#include "Compress.h"
#include "SharedMemory.h"
#include "FileReadWrite.h"

#include "MultiKey.h"
#include "MathWorker.h"

#define LOGGING(...)
//#define LOGGING printf

// TJD NOTE: I question whether we need this entire file
//------------------------------------------------------------
static size_t CompressGetBound(int compMode, size_t srcSize) {
   if (compMode == COMPRESSION_TYPE_ZSTD) {
      return ZSTD_compressBound(srcSize);
   }

   printf("!!internal CompressBound error\n");
   return srcSize;
}


//------------------------------------------------------------
static size_t CompressData(int compMode, void* dst, size_t dstCapacity,
   const void* src, size_t srcSize,
   int compressionLevel) {

   if (compMode == COMPRESSION_TYPE_ZSTD) {
      return ZSTD_compress(dst, dstCapacity, src, srcSize, compressionLevel);
   }

   printf("!!internal CompressData error\n");
   return srcSize;

}

//------------------------------------------------------------
static size_t DecompressData(int compMode, void* dst, size_t dstCapacity, const void* src, size_t srcSize) {

   return ZSTD_decompress(dst, dstCapacity, src, srcSize);
}


//------------------------------------------------------------
static BOOL CompressIsError(int compMode, size_t code) {
   if (ZSTD_isError(code)) {
      PyErr_Format(PyExc_ValueError, "Decompression error: %s", ZSTD_getErrorName(code));
      return TRUE;
   }
   else {
      return FALSE;
   }

}


//------------------------------------------------------------


//--------------------------
// Used when compressing from one numpy array to another numpy array
//
void FillInNumpyHeader(NUMPY_HEADERSIZE* pstNumpyHeader, INT32 dtype, INT32 ndim, INT64* dims, INT32 flags, INT64 itemsize) {
   if (pstNumpyHeader) {
      pstNumpyHeader->magic = COMPRESSION_MAGIC;
      pstNumpyHeader->compressiontype = COMPRESSION_TYPE_ZSTD;

      pstNumpyHeader->dtype = (INT8)(dtype);
      pstNumpyHeader->ndim = (INT8)ndim;

      pstNumpyHeader->flags = flags;
      pstNumpyHeader->itemsize = (INT32)itemsize;

      if (pstNumpyHeader->ndim > 3) {
         printf("!!! too many dimensions\n");
         pstNumpyHeader->ndim = 3;
      }

      for (int i = 0; i < pstNumpyHeader->ndim; i++) {
         pstNumpyHeader->dimensions[i] = dims[i];
      }
   }

}


//----------------------------------------------------
// Returns temporary array to compress into
// Caller is responsible for freeing memory
NUMPY_HEADERSIZE* AllocCompressedMemory(INT64 arraylength, INT32 dtype, INT32 ndim, INT64* dims, INT32 flags, INT64 itemsize) {

   INT64 source_size = arraylength * itemsize;
   INT64 dest_size = ZSTD_compressBound(source_size);

   // Include enough for our header
   NUMPY_HEADERSIZE* pstNumpyHeader = (NUMPY_HEADERSIZE*)WORKSPACE_ALLOC(dest_size + sizeof(NUMPY_HEADERSIZE));

   FillInNumpyHeader(pstNumpyHeader, dtype, ndim, dims, flags, itemsize);

   return pstNumpyHeader;
}


//-------------------------------------------------------
//Decompress
BOOL DecompressOneArray(void* pstCompressArraysV, int core, INT64 t) {
   COMPRESS_NUMPY_TO_NUMPY* pstCompressArrays = (COMPRESS_NUMPY_TO_NUMPY *)pstCompressArraysV;
   ArrayInfo* aInfo = pstCompressArrays->aInfo;

   if (pstCompressArrays->pNumpyHeaders[t]) {
      NUMPY_HEADERSIZE* pNumpyHeader = (NUMPY_HEADERSIZE*)pstCompressArrays->aInfo[t].pData;

      void* source = &pNumpyHeader[1];
      UINT64 dest_size = (UINT64)ZSTD_getDecompressedSize(source, pNumpyHeader->compressedSize);

      if (dest_size == 0) {
         PyErr_Format(PyExc_ValueError, "input data invalid or missing content size in frame header");
         return FALSE;
      }

      PyArrayObject* pArrayObject = pstCompressArrays->pNumpyArray[t];
      void* dest = PyArray_BYTES(pArrayObject);

      size_t cSize = ZSTD_decompress(dest, dest_size, source, pNumpyHeader->compressedSize);

      if (ZSTD_isError(cSize)) {
         PyErr_Format(PyExc_ValueError, "Decompression error: %s", ZSTD_getErrorName(cSize));
         return FALSE;
      }
      else if (cSize != dest_size) {
         PyErr_Format(PyExc_ValueError, "Decompression error: length mismatch -> decomp %llu != %llu [header]", (UINT64)cSize, dest_size);
         return FALSE;
      }
   }

   return TRUE;
}



//-------------------------------------------------------
//Compress -- called from multiple threads
BOOL CompressMemoryArray(void* pstCompressArraysV, int core, INT64 t) {
   COMPRESS_NUMPY_TO_NUMPY* pstCompressArrays = (COMPRESS_NUMPY_TO_NUMPY *)pstCompressArraysV;
   ArrayInfo* aInfo = pstCompressArrays->aInfo;

   if (pstCompressArrays->compMode == COMPRESSION_MODE_COMPRESS && pstCompressArrays->pNumpyHeaders[t]) {
      NUMPY_HEADERSIZE* pstNumpyHeader = pstCompressArrays->pNumpyHeaders[t];

      INT64 source_size = aInfo[t].ArrayLength * aInfo[t].ItemSize;
      INT64 dest_size = ZSTD_compressBound(source_size);

      LOGGING("[%d] started %lld %p\n", (int)t, source_size, pstNumpyHeader);

      // data to compress is after the header
      size_t cSize = ZSTD_compress(&pstNumpyHeader[1], dest_size, aInfo[t].pData, source_size, pstCompressArrays->compLevel);

      pstNumpyHeader->compressedSize = cSize;

      LOGGING("[%d] compressed %d%% %llu from %lld\n", (int)t, (int)((cSize * 100) / source_size), cSize, source_size);

      if (ZSTD_isError(cSize)) {
         printf("ZSTD_isError\n");
         WORKSPACE_FREE(pstCompressArrays->pNumpyHeaders[t]);
         pstCompressArrays->pNumpyHeaders[t] = NULL;
      }
   }

   return TRUE;
}



//----------------------------------------------------
// Arg1: Pass in list of arrays
// Arg2: Mode == compress or decompress
// Arg3: <optional> compression level
// Returns list of arrays compressed
//
PyObject *CompressDecompressArrays(PyObject* self, PyObject *args)
{
   PyObject *inList1 = NULL;
   INT32 level = ZSTD_CLEVEL_DEFAULT;
   INT32 mode = COMPRESSION_MODE_COMPRESS;

   if (!PyArg_ParseTuple(
      args, "Oi|i",
      &inList1,
      &mode,
      &level)) {

      return NULL;
   }

   INT64 totalItemSize = 0;
   INT64 tupleSize = 0;
   ArrayInfo* aInfo = BuildArrayInfo(inList1, &tupleSize, &totalItemSize, mode == COMPRESSION_MODE_COMPRESS);

   if (aInfo) {

      COMPRESS_NUMPY_TO_NUMPY* pstCompressArrays = (COMPRESS_NUMPY_TO_NUMPY*)WORKSPACE_ALLOC(sizeof(COMPRESS_NUMPY_TO_NUMPY) + (tupleSize * sizeof(NUMPY_HEADERSIZE*)));
      pstCompressArrays->totalHeaders = tupleSize;

      //---------------------
      if (level <= 0) level = ZSTD_CLEVEL_DEFAULT;
      if (level > ZSTD_MAX_CLEVEL) level = ZSTD_MAX_CLEVEL;

      pstCompressArrays->compLevel = level;
      pstCompressArrays->compType = COMPRESSION_TYPE_ZSTD;
      pstCompressArrays->compMode = (INT16)mode;
      //pstCompressArrays->pBlockInfo = NULL;
      pstCompressArrays->aInfo = aInfo;

      INT numCores = g_cMathWorker->WorkerThreadCount + 1;

      switch (mode) {
      case COMPRESSION_MODE_COMPRESS:
      {
         // Change this per CORE
         // Allocate worst care
         for (int t = 0; t < tupleSize; t++) {

            pstCompressArrays->pNumpyHeaders[t] =
               AllocCompressedMemory(
                  aInfo[t].ArrayLength,
                  aInfo[t].NumpyDType,
                  aInfo[t].NDim,
                  (INT64*)(((PyArrayObject_fields *)aInfo[t].pObject)->dimensions),
                  PyArray_FLAGS(aInfo[t].pObject),
                  aInfo[t].ItemSize);

         }

         // This will kick off the workerthread and call CompressMemoryArray passing pstCompressArrays as argument with counter and core
         g_cMathWorker->DoMultiThreadedWork((int)tupleSize, CompressMemoryArray, pstCompressArrays);
      }
      break;

      case COMPRESSION_MODE_DECOMPRESS:
      {
         for (int t = 0; t < tupleSize; t++) {
            NUMPY_HEADERSIZE* pNumpyHeader = (NUMPY_HEADERSIZE*)aInfo[t].pData;
            UINT64 dest_size = (UINT64)ZSTD_getDecompressedSize(&pNumpyHeader[1], pNumpyHeader->compressedSize);

            // Allocate all the arrays before multithreading
            // NOTE: do we care about flags -- what if Fortran mode when saved?
            pstCompressArrays->pNumpyArray[t] =
               AllocateNumpyArray(pNumpyHeader->ndim, (npy_intp*)pNumpyHeader->dimensions, pNumpyHeader->dtype, pNumpyHeader->itemsize);
            CHECK_MEMORY_ERROR(pstCompressArrays->pNumpyArray[t]);

         }

         g_cMathWorker->DoMultiThreadedWork((int)tupleSize, DecompressOneArray, pstCompressArrays);
      }
      break;
      }


      //--------------------
      // Return all the compressed arrays as UINT8
      // For decompressed, return original array
      // New reference
      PyObject* returnTuple = Py_None;

      if (mode == COMPRESSION_MODE_COMPRESS_FILE) {
         for (int j = 0; j < numCores; j++) {
            if (pstCompressArrays->pCoreMemory[j]) {
               WORKSPACE_FREE(pstCompressArrays->pCoreMemory[j]);
            }
         }
      }

      if (mode == COMPRESSION_MODE_COMPRESS) {
         returnTuple = PyTuple_New(tupleSize);
         INT64 uncompressedSize = 0;
         INT64 compressedSize = 0;

         //--------------------
         // Fill in results
         // Now we know actual length since compression is completed
         // Copy into a real array
         for (int t = 0; t < tupleSize; t++) {
            PyObject* item = NULL;

            if (pstCompressArrays->pNumpyHeaders[t]) {

               // make numpy arrays
               NUMPY_HEADERSIZE* pstNumpyHeader = pstCompressArrays->pNumpyHeaders[t];

               char* pCompressedData = (char*)(&pstNumpyHeader[1]);

               INT64 totalCompressedBytes = pstNumpyHeader->compressedSize + sizeof(NUMPY_HEADERSIZE);
               INT64 uncomp = (INT64)ZSTD_getDecompressedSize(pCompressedData, (size_t)totalCompressedBytes);

               uncompressedSize += uncomp;
               compressedSize += totalCompressedBytes;

               item = (PyObject*)AllocateNumpyArray(1, (npy_intp*)&totalCompressedBytes, NPY_UINT8);
               CHECK_MEMORY_ERROR(item);

               // make sure we got memory
               if (item) {

                  // tag as not writeable
                  ((PyArrayObject_fields *)item)->flags &= ~NPY_ARRAY_WRITEABLE;

                  void* pDest = PyArray_BYTES((PyArrayObject*)item);
                  memcpy(pDest, pstNumpyHeader, totalCompressedBytes);
               }

               // Free memory we allocated for worst case
               WORKSPACE_FREE(pstNumpyHeader);

            }

            // Return NONE for any arrays with memory issues
            if (item == NULL) {
               item = Py_None;
               Py_INCREF(Py_None);
            }

            //printf("ref %d  %llu\n", i, item->ob_refcnt);
            PyTuple_SET_ITEM(returnTuple, t, item);
         }
         LOGGING("%d%% comp ratio    compressed size: %lld    uncompressed size: %lld\n", (int)((compressedSize * 100) / uncompressedSize), compressedSize, uncompressedSize);
      }
      if (mode == COMPRESSION_MODE_DECOMPRESS) {
         returnTuple = PyTuple_New(tupleSize);

         // Decompression
         for (int t = 0; t < tupleSize; t++) {
            PyObject* item = NULL;

            if (pstCompressArrays->pNumpyHeaders[t]) {
               item = (PyObject*)pstCompressArrays->pNumpyArray[t];
            }

            // Return NONE for any arrays with memory issues
            if (item == NULL) {
               item = Py_None;
               Py_INCREF(Py_None);
            }

            //printf("ref %d  %llu\n", i, item->ob_refcnt);
            PyTuple_SET_ITEM(returnTuple, t, item);
         }
      }

      WORKSPACE_FREE(pstCompressArrays);
      FreeArrayInfo(aInfo);

      return returnTuple;
   }

   return NULL;
}



//----------------------------------------------------
// Compress a byte string
PyObject *CompressString(PyObject* self, PyObject *args)
{

   PyObject *result;
   const char *source;
   UINT32 source_size32;
   char *dest;
   size_t source_size;
   size_t dest_size;
   size_t cSize;
   INT32 level = ZSTD_CLEVEL_DEFAULT;

   if (!PyArg_ParseTuple(args, "y#|i", &source, &source_size32, &level))
      return NULL;

   if (level <= 0) level = ZSTD_CLEVEL_DEFAULT;

   if (level > ZSTD_MAX_CLEVEL) level = ZSTD_MAX_CLEVEL;

   source_size = source_size32;

   dest_size = ZSTD_compressBound(source_size);
   result = PyBytes_FromStringAndSize(NULL, dest_size);
   if (result == NULL) {
      return NULL;
   }

   if (source_size > 0) {
      dest = PyBytes_AS_STRING(result);

      printf("trying to compress %s  size:%zu  dest_size:%zu %d\n", source, source_size, dest_size, level);

      cSize = ZSTD_compress(dest, dest_size, source, source_size, level);

      printf("compressed to %zu\n", cSize);

      if (ZSTD_isError(cSize)) {
         PyErr_Format(PyExc_ValueError, "Compression error: %s", ZSTD_getErrorName(cSize));
         Py_CLEAR(result);
         return NULL;
      }

      Py_SIZE(result) = cSize;
   }
   return result;
}

/**
* New more interoperable function
* Uses origin zstd header, nothing more
* Simple version: not for streaming, no dict support, full block decompression
*/
PyObject *DecompressString(PyObject* self, PyObject *args)
{

   PyObject    *result;
   const char  *source;
   UINT32      source_size32;
   size_t      source_size;
   UINT64      dest_size;
   char        error = 0;
   size_t      cSize;

   if (!PyArg_ParseTuple(args, "y#", &source, &source_size32))
      return NULL;

   source_size = source_size32;

   dest_size = (UINT64)ZSTD_getDecompressedSize(source, source_size);
   if (dest_size == 0) {
      PyErr_Format(PyExc_ValueError, "input data invalid or missing content size in frame header");
      return NULL;
   }
   result = PyBytes_FromStringAndSize(NULL, dest_size);

   if (result != NULL) {
      char *dest = PyBytes_AS_STRING(result);

      cSize = ZSTD_decompress(dest, dest_size, source, source_size);

      if (ZSTD_isError(cSize)) {
         PyErr_Format(PyExc_ValueError, "Decompression error: %s", ZSTD_getErrorName(cSize));
         error = 1;
      }
      else if (cSize != dest_size) {
         PyErr_Format(PyExc_ValueError, "Decompression error: length mismatch -> decomp %llu != %llu [header]", (UINT64)cSize, dest_size);
         error = 1;
      }
   }

   if (error) {
      Py_CLEAR(result);
      result = NULL;
   }

   return result;
}

