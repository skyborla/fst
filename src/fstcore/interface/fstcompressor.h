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


#ifndef FST_COMPRESSOR_H
#define FST_COMPRESSOR_H


#include <stdlib.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "compression/compressor.h"
#include "interface/fstdefines.h"
#include "interface/itypefactory.h"
#include "interface/openmphelper.h"

#include "ZSTD/common/xxhash.h"

enum COMPRESSION_ALGORITHM
{
	ALGORITHM_NONE = 0,
	ALGORITHM_LZ4,
	ALGORITHM_ZSTD
};


class FstCompressor
{
	Compressor* compressor;
	ITypeFactory* typeFactory;
	COMPRESSION_ALGORITHM compAlgorithm;

public:

	FstCompressor(ITypeFactory* typeFactory)
	{
		this->typeFactory = typeFactory;
		compressor = nullptr;  // only use this class for decompression (not checked!)
	}

	FstCompressor(COMPRESSION_ALGORITHM compAlgorithm, unsigned int compressionLevel, ITypeFactory* typeFactory)
	{
		this->typeFactory = typeFactory;
		this->compAlgorithm = compAlgorithm;

		switch (compAlgorithm)
		{
			case COMPRESSION_ALGORITHM::ALGORITHM_LZ4:
			{
				compressor = new SingleCompressor(CompAlgo::LZ4, compressionLevel);
				break;
			}

			case COMPRESSION_ALGORITHM::ALGORITHM_ZSTD:
			{
				compressor = new SingleCompressor(CompAlgo::ZSTD, compressionLevel);
				break;
			}

			default:
			{
				compressor = new SingleCompressor(CompAlgo::LZ4, compressionLevel);
				break;
			}
		}
	}


	/**
	 * \brief Compress data with the 'LZ4' or 'ZSTD' compressor.
	 * \param blobSource source buffer to compress.
	 * \param blobLength Length of source buffer.
	 */
	IBlobContainer* CompressBlob(unsigned char* blobSource, unsigned long long blobLength, bool hash = true) const
	{
		int nrOfThreads = GetFstThreads();

		// Check for empty source
		if (blobLength == 0)
		{
			throw(std::runtime_error("Source contains no data."));
		}


		// block size to use for compression has a lower bound for compression efficiency
		unsigned long long minBlock = std::max(static_cast<unsigned long long>(BLOCKSIZE),
			1 + (blobLength - 1) / PREV_NR_OF_BLOCKS);

		// And a higher bound for compressor compatability
		unsigned int blockSize = static_cast<unsigned int>(std::min(minBlock, static_cast<unsigned long long>(INT_MAX)));

		int nrOfBlocks = static_cast<int>(1 + (blobLength - 1) / blockSize);
		nrOfThreads = std::min(nrOfThreads, nrOfBlocks);

		unsigned int maxCompressSize = this->compressor->CompressBufferSize(blockSize);
		unsigned int lastBlockSize = 1 + (blobLength - 1) % blockSize;

		unsigned long long bufSize = nrOfBlocks * maxCompressSize;
		float blocksPerThread = static_cast<float>(nrOfBlocks) / nrOfThreads;

		// Compressed sizes
		unsigned long long* compSizes = new unsigned long long[nrOfBlocks + 1];
		unsigned long long* compBatchSizes = new unsigned long long[nrOfThreads];

		unsigned int* blockHashes = nullptr;

		if (hash)
		{
			blockHashes = new unsigned int[nrOfBlocks];
		}

		// We need nrOfBlocks buffers
		unsigned int compressionAlgo;
		unsigned char* calcBuffer = new unsigned char[bufSize];

#pragma omp parallel num_threads(nrOfThreads) shared(compSizes,nrOfThreads,blocksPerThread,maxCompressSize,lastBlockSize,compBatchSizes)
		{
#pragma omp for schedule(static, 1) nowait
			for (int blockBatch = 0; blockBatch < (nrOfThreads - 1); blockBatch++)  // all but last batch
			{
				CompAlgo compAlgo;
				float blockOffset = blockBatch * blocksPerThread;
				int blockNr = static_cast<int>(0.00001 + blockOffset);
				int nextblockNr = static_cast<int>(blocksPerThread + 0.00001 + blockOffset);

				unsigned char* threadBuf = calcBuffer + maxCompressSize * blockNr;  // buffer for compression results of current thread

				// inner loop is executed by current thread
				unsigned long long bufPos = 0;
				for (int block = blockNr; block < nextblockNr; block++)
				{
					int compSize = this->compressor->Compress(reinterpret_cast<char*>(threadBuf + bufPos), maxCompressSize,
						reinterpret_cast<char*>(&blobSource[block * blockSize]), blockSize, compAlgo);
					compSizes[block] = static_cast<unsigned long long>(compSize);

					// Hash compression result
					if(hash)
					{
						blockHashes[block] = XXH32(threadBuf + bufPos, compSize, FST_HASH_SEED);
					}

					bufPos += compSize;
				}

				compBatchSizes[blockBatch] = bufPos;
			}

#pragma omp single
			{
				CompAlgo compAlgo;
				int blockNr = static_cast<int>(0.00001 + (nrOfThreads - 1) * blocksPerThread);
				int nextblockNr = static_cast<int>(0.00001 + (nrOfThreads * blocksPerThread)) - 1;  // exclude last block

				unsigned char* threadBuf = calcBuffer + maxCompressSize * blockNr;  // buffer for compression results of current thread

				// inner loop is executed by current thread
				unsigned long long bufPos = 0;
				for (int block = blockNr; block < nextblockNr; block++)
				{
					int compSize = this->compressor->Compress(reinterpret_cast<char*>(threadBuf + bufPos), maxCompressSize,
						reinterpret_cast<char*>(&blobSource[block * blockSize]), blockSize, compAlgo);
					compSizes[block] = static_cast<unsigned long long>(compSize);

					// Hash compression result
					if (hash)
					{
						blockHashes[block] = XXH32(threadBuf + bufPos, compSize, FST_HASH_SEED);
					}

					bufPos += compSize;
				}

				// last block
				int compSize = this->compressor->Compress(reinterpret_cast<char*>(threadBuf + bufPos), maxCompressSize,
					reinterpret_cast<char*>(&blobSource[nextblockNr * blockSize]), lastBlockSize, compAlgo);
				compSizes[nextblockNr] = static_cast<unsigned long long>(compSize);

				compBatchSizes[nrOfThreads - 1] = bufPos + compSize;
				compressionAlgo = static_cast<unsigned int>(compAlgo);

				// Hash compression result
				if (hash)
				{
					blockHashes[nextblockNr] = XXH32(threadBuf + bufPos, compSize, FST_HASH_SEED);
				}

			}

		}  // end parallel region and join all threads


		unsigned int allBlockHash = 0;

		if (hash)
		{
			allBlockHash = XXH32(blockHashes, nrOfBlocks * 4, FST_HASH_SEED);
			delete[] blockHashes;
		}

		unsigned long long totCompSize = 0;
		for (int blockBatch = 0; blockBatch < nrOfThreads; blockBatch++)
		{
			totCompSize += compBatchSizes[blockBatch];
		}

		// In memory compression format:
		// Size                 | Type               | Description
		// 8                    | unsigned long long | fst marker
		// 4                    | insigned int       | header hash
		// 4                    | unsigned int       | blockSize
		// 4                    | unsigned int       | version
		// 4                    | unsigned int       | COMPRESSION_ALGORITHM and upper bit isHashed
		// 8                    | unsigned long long | vecLength
		// 4                    | unsigned int       | block hash result
		// 8 * (nrOfBlocks + 1) | unsigned long long | block offset
		//                      | unsigned char      | compressed data

		unsigned long long headerSize = 4 + 8 * (nrOfBlocks + 5);
		IBlobContainer* blobContainer = typeFactory->CreateBlobContainer(headerSize + totCompSize);
		unsigned char* blobData = blobContainer->Data();

		unsigned long long* fstMarker = reinterpret_cast<unsigned long long*>(blobData);
		unsigned int* headerHash = reinterpret_cast<unsigned int*>(blobData + 8);
		unsigned int* pBlockSize = reinterpret_cast<unsigned int*>(blobData + 12);
		unsigned int* version = reinterpret_cast<unsigned int*>(blobData + 16);
		unsigned int* algo = reinterpret_cast<unsigned int*>(blobData + 20);
		unsigned long long* vecLength = reinterpret_cast<unsigned long long*>(blobData + 24);
		unsigned int* hashResult = reinterpret_cast<unsigned int*>(blobData + 32);
		unsigned long long* blockOffsets = reinterpret_cast<unsigned long long*>(blobData + 36);

		*fstMarker = FST_FILE_ID;
		*pBlockSize = blockSize;
		*version = 1;
		*algo = compressionAlgo | ((1 << 31) * hash);  // upper bit signals isHashed
		*vecLength = blobLength;
		*hashResult = allBlockHash;

		// calculate offsets for memcpy
		unsigned long long dataOffset = headerSize;
		unsigned long long* dataOffsets = new unsigned long long[nrOfThreads];
		for (int blockBatch = 0; blockBatch < nrOfThreads; blockBatch++)
		{
			dataOffsets[blockBatch] = dataOffset;
			dataOffset += compBatchSizes[blockBatch];
		}

		// multi-threaded memcpy
#pragma omp parallel for schedule(static, 1)
		for (int blockBatch = 0; blockBatch < nrOfThreads; blockBatch++)
		{
			float blockOffset = blockBatch * blocksPerThread;
			int blockNr = static_cast<int>(0.00001 + blockOffset);
			unsigned char* threadBuf = calcBuffer + maxCompressSize * blockNr;  // buffer for compression results of current thread
			std::memcpy(blobData + dataOffsets[blockBatch], threadBuf, compBatchSizes[blockBatch]);
		}  // end parallel region

		delete[] compBatchSizes;
		delete[] calcBuffer;
		delete[] dataOffsets;

		unsigned long long blockOffset = headerSize;
		for (int block = 0; block < nrOfBlocks; block++)
		{
			blockOffsets[block] = blockOffset;
			blockOffset += compSizes[block];
		}
		blockOffsets[nrOfBlocks] = blockOffset;

		delete[] compSizes;

		*headerHash = XXH32(&blobData[12], headerSize - 12, FST_HASH_SEED);  // header hash

		return blobContainer;
	}

	// In memory compression format:
	// Size                 | Type               | Description
	// 8                    | unsigned long long | fst marker
	// 4                    | insigned int       | header hash
	// 4                    | unsigned int       | blockSize
	// 4                    | unsigned int       | version
	// 4                    | unsigned int       | COMPRESSION_ALGORITHM and upper bit isHashed
	// 8                    | unsigned long long | vecLength
	// 4                    | unsigned int       | block hash result
	// 8 * (nrOfBlocks + 1) | unsigned long long | block offset
	//                      | unsigned char      | compressed data

	IBlobContainer* DecompressBlob(unsigned char* blobSource, unsigned long long blobLength, bool checkHashes = true) const
	{
		Decompressor decompressor;
		int nrOfThreads = GetFstThreads();  // available threads

		// Minimum length of compressed data format
		if (blobLength < 45)
		{
			throw(std::runtime_error("Data format not recognised as compressed fst format, compressed size is too small."));
		}

		// Meta data of compressed blocks

		unsigned long long* fstMarker = reinterpret_cast<unsigned long long*>(blobSource);
		unsigned int* headerHash = reinterpret_cast<unsigned int*>(blobSource + 8);
		unsigned int* blockSize = reinterpret_cast<unsigned int*>(blobSource + 12);
		//unsigned int* version = reinterpret_cast<unsigned int*>(blobSource + 16);
		unsigned int* algo = reinterpret_cast<unsigned int*>(blobSource + 20);
		unsigned long long* vecLength = reinterpret_cast<unsigned long long*>(blobSource + 24);
		unsigned int* hashResult = reinterpret_cast<unsigned int*>(blobSource + 32);
		unsigned long long* blockOffsets = reinterpret_cast<unsigned long long*>(blobSource + 36);

		bool hash = checkHashes && static_cast<bool>(((*algo) >> 31) & 1);
		unsigned int algorithm = (*algo) & 0x7fffffff;  // highest bit signals hash

		// Check fst magic marker
		if (*fstMarker != FST_FILE_ID)
		{
			throw(std::runtime_error("Data format is not recognised as compressed fst format."));
		}

		// Calculate number of blocks
		int nrOfBlocks = static_cast<int>(1 + (*vecLength - 1) / *blockSize);  // including (partial) last

		unsigned long long headerSize = 4 + 8 * (static_cast<unsigned long long>(nrOfBlocks) + 5);

		// Minimum length of compressed data format including block offset header information
		if (blobLength <= headerSize)
		{
			throw(std::runtime_error("Compressed data vector has incorrect size."));
		}

		unsigned int headHash = XXH32(&blobSource[12], headerSize - 12, FST_HASH_SEED);  // header hash

		if (*headerHash != headHash)
		{
			throw(std::runtime_error("Incorrect header information found in raw vector."));
		}


		// Version checks here

		// Create result blob
		IBlobContainer* blobContainer = typeFactory->CreateBlobContainer(*vecLength);  // create result blob
		unsigned char* blobData = blobContainer->Data();

		// Source vector has correct length
		if (blockOffsets[nrOfBlocks] != blobLength)
		{
			delete blobContainer;
			throw(std::runtime_error("Compressed data vector has incorrect size."));
		}

		// Determine required number of threads
		nrOfThreads = std::min(nrOfBlocks, nrOfThreads);

		// Determine number of blocks per (thread) batch
		float batchFactor = static_cast<float>(nrOfBlocks) / nrOfThreads;

		bool error = false;

		if (hash)
		{
			unsigned int* blockHashes = new unsigned int[nrOfBlocks];

#pragma omp parallel num_threads(nrOfThreads)
			{
#pragma omp for schedule(static, 1) nowait
				for (int batch = 0; batch < (nrOfThreads - 1); batch++)  // all but last batch
				{
					int fromBlock = static_cast<int>(batch * batchFactor + 0.000001);  // start block
					int toBlock = static_cast<int>((batch + 1) * batchFactor + 0.000001);  // end block

				   // iterate block range
					for (int block = fromBlock; block < toBlock; block++)
					{
						unsigned long long blockStart = blockOffsets[block];
						unsigned long long blockEnd = blockOffsets[block + 1];

						blockHashes[block] = XXH32(blobSource + blockStart, blockEnd - blockStart, FST_HASH_SEED);
					}
				}

#pragma omp single
				{
					int fromBlock = static_cast<int>((nrOfThreads - 1) * batchFactor + 0.000001);  // start block
					int toBlock = static_cast<int>(nrOfThreads * batchFactor + 0.000001);  // end block

				   // iterate block range with full blocks
					for (int block = fromBlock; block < (toBlock - 1); block++)
					{
						unsigned long long blockStart = blockOffsets[block];
						unsigned long long blockEnd = blockOffsets[block + 1];

						blockHashes[block] = XXH32(blobSource + blockStart, blockEnd - blockStart, FST_HASH_SEED);
					}

					// last block
					unsigned long long blockStart = blockOffsets[toBlock - 1];
					unsigned long long blockEnd = blockOffsets[toBlock];

					blockHashes[toBlock - 1] = XXH32(blobSource + blockStart, blockEnd - blockStart, FST_HASH_SEED);
				}
			}

			unsigned int totHashes = XXH32(blockHashes, 4 * nrOfBlocks, FST_HASH_SEED);
			delete[] blockHashes;

			if (totHashes != *hashResult)
			{
				delete blobContainer;
				throw(std::runtime_error("Incorrect input vector: data block hash does not match."));
			}
		}

#pragma omp parallel num_threads(nrOfThreads)
		{
#pragma omp for schedule(static, 1) nowait
			for (int batch = 0; batch < (nrOfThreads - 1); batch++)  // all but last batch
			{
				int fromBlock = static_cast<int>(batch * batchFactor + 0.000001);  // start block
				int toBlock = static_cast<int>((batch + 1) * batchFactor + 0.000001);  // end block

				// iterate block range
				for (int block = fromBlock; block < toBlock; block++)
				{
					unsigned long long blockStart = blockOffsets[block];
					unsigned long long blockEnd = blockOffsets[block + 1];

					unsigned int errorCode = decompressor.Decompress(algorithm, reinterpret_cast<char*>(blobData) + *blockSize * block,
						*blockSize, reinterpret_cast<const char*>(blobSource + blockStart), blockEnd - blockStart);

					if (errorCode != 0)
					{
						error = true;
					}
				}
			}

#pragma omp single
			{
				int fromBlock = static_cast<int>((nrOfThreads - 1) * batchFactor + 0.000001);  // start block
				int toBlock = static_cast<int>(nrOfThreads * batchFactor + 0.000001);  // end block

				// iterate block range with full blocks
				for (int block = fromBlock; block < (toBlock - 1); block++)
				{
					unsigned long long blockStart = blockOffsets[block];
					unsigned long long blockEnd = blockOffsets[block + 1];
					unsigned int errorCode = decompressor.Decompress(algorithm, reinterpret_cast<char*>(blobData) + *blockSize * block, *blockSize,
						reinterpret_cast<const char*>(blobSource + blockStart), blockEnd - blockStart);

					if (errorCode != 0)
					{
						error = true;
					}
				}

				// last block
				int lastBlockSize = 1 + (*vecLength - 1) % *blockSize;
				unsigned long long blockStart = blockOffsets[toBlock - 1];
				unsigned long long blockEnd = blockOffsets[toBlock];
				unsigned int errorCode = decompressor.Decompress(algorithm, reinterpret_cast<char*>(blobData) + *blockSize * (toBlock - 1), lastBlockSize,
					reinterpret_cast<const char*>(blobSource + blockStart), blockEnd - blockStart);

				if (errorCode != 0)
				{
					error = true;
				}
			}
		}


		if (error)
		{
			delete blobContainer;
			throw(std::runtime_error("An error was detected in the compressed data stream."));
		}

		return blobContainer;
	}

};


#endif  // FST_COMPRESSOR_H
