/*
  fst - An R-package for ultra fast storage and retrieval of datasets.
  Copyright (C) 2017, Mark AJ Klik

  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You can contact the author at :
  - fst source repository : https://github.com/fstPackage/fst
*/


#include <Rcpp.h>

#include <interface/fstcompressor.h>
#include <interface/fsthash.h>

#include <typefactory.h>

using namespace Rcpp;


SEXP fsthasher(SEXP rawVec, SEXP seed)
{
  FstHasher hasher;

  SEXP res = PROTECT(Rf_allocVector(INTSXP, 1));

  unsigned int* uintP = (unsigned int*)(INTEGER(res));

  *uintP = 5;

  if (Rf_isNull(seed))
  {
    *uintP = hasher.HashBlob((unsigned char*) RAW(rawVec), Rf_length(rawVec));

    UNPROTECT(1);
    return res;
  }

  *uintP = hasher.HashBlob((unsigned char*) RAW(rawVec), Rf_length(rawVec),
    *((unsigned int*) INTEGER(seed)));

  UNPROTECT(1);
  return res;
}


SEXP fstcomp(SEXP rawVec, SEXP compressor, SEXP compression, SEXP hash)
{
  ITypeFactory* typeFactory = new TypeFactory();
  COMPRESSION_ALGORITHM algo;

  if (!Rf_isLogical(hash))
  {
    delete typeFactory;
    Rf_error("Please specify true of false for parameter hash.");
  }

  if (Rf_NonNullStringMatch(STRING_ELT(compressor, 0), Rf_mkChar("LZ4")))
  {
    algo = COMPRESSION_ALGORITHM::ALGORITHM_LZ4;
  } else if (Rf_NonNullStringMatch(STRING_ELT(compressor, 0), Rf_mkChar("ZSTD")))
  {
    algo = COMPRESSION_ALGORITHM::ALGORITHM_ZSTD;
  } else
  {
    delete typeFactory;
    Rf_error("Unknown compression algorithm selected");
  }

  FstCompressor fstcompressor(algo, *INTEGER(compression), (ITypeFactory*) typeFactory);

  unsigned long long vecLength = Rf_xlength(rawVec);
  unsigned char* data = (unsigned char*) RAW(rawVec);
  BlobContainer* blobContainer;

  try
  {
    blobContainer = (BlobContainer*) fstcompressor.CompressBlob(data, vecLength, *LOGICAL(hash));
  }
  catch(const std::runtime_error& e)
  {
    delete typeFactory;
    ::Rf_error(e.what());
  }
  catch ( ... )
  {
    delete typeFactory;
    ::Rf_error("Unexpected error detected while compressing data.");
  }

  SEXP resVec = blobContainer->RVector();

  delete typeFactory;
  delete blobContainer;

  return resVec;
}


SEXP fstdecomp(SEXP rawVec)
{
  ITypeFactory* typeFactory = new TypeFactory();

  FstCompressor fstcompressor((ITypeFactory*) typeFactory);

  unsigned long long vecLength = Rf_xlength(rawVec);
  unsigned char* data = (unsigned char*) (RAW(rawVec));

  BlobContainer* resultContainer;

  try
  {
    resultContainer = static_cast<BlobContainer*>(fstcompressor.DecompressBlob(data, vecLength));
  }
  catch(const std::runtime_error& e)
  {
    delete typeFactory;
    ::Rf_error(e.what());
  }
  catch ( ... )
  {
    delete typeFactory;
    ::Rf_error("Error detected while decompressing data.");
  }

  SEXP resVec = resultContainer->RVector();

  delete typeFactory;
  delete resultContainer;

  return resVec;
}

