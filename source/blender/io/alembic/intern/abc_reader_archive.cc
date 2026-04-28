/* SPDX-FileCopyrightText: 2016 Kévin Dietrich. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup balembic
 */

#include "abc_reader_archive.h"

#include "Alembic/Abc/ArchiveInfo.h"
#include "Alembic/AbcCoreAbstract/MetaData.h"
#include "Alembic/AbcCoreLayer/Read.h"
#include "Alembic/AbcCoreOgawa/ReadWrite.h"

#include "BKE_main.hh"

#include "BLI_path_utils.hh"
#include "BLI_string.h"

#ifdef WIN32
#  include "utfconv.hh"
#endif

#include <fstream>
#include <vector>

namespace blender {

using Alembic::Abc::chrono_t;
using Alembic::Abc::ErrorHandler;
using Alembic::Abc::Exception;
using Alembic::Abc::IArchive;
using Alembic::Abc::index_t;
using Alembic::Abc::kWrapExisting;
using Alembic::Abc::MetaData;
using Alembic::Abc::TimeSamplingPtr;
using Alembic::Abc::TimeSamplingType;

namespace io::alembic {

static IArchive open_archive(const std::string &filename,
                             const std::vector<std::istream *> &input_streams)
{
  try {
    Alembic::AbcCoreOgawa::ReadArchive archive_reader(input_streams);

    return IArchive(archive_reader(filename), kWrapExisting, ErrorHandler::kThrowPolicy);
  }
  catch (const Exception &e) {
    std::cerr << e.what() << '\n';

    /* Inspect the file to see whether it's actually a HDF5 file. */
    char header[4]; /* char(0x89) + "HDF" */
    std::ifstream the_file(filename.c_str(), std::ios::in | std::ios::binary);
    if (!the_file) {
      std::cerr << "Unable to open " << filename << std::endl;
    }
    else if (!the_file.read(header, sizeof(header))) {
      std::cerr << "Unable to read from " << filename << std::endl;
    }
    else if (strncmp(header + 1, "HDF", 3) != 0) {
      std::cerr << filename << " has an unknown file format, unable to read." << std::endl;
    }
    else {
      std::cerr << filename << " is in the obsolete HDF5 format, unable to read." << std::endl;
    }

    if (the_file.is_open()) {
      the_file.close();
    }
  }

  return IArchive();
}

ArchiveReader *ArchiveReader::get(const Main *bmain, const std::vector<const char *> &filenames)
{
  std::vector<ArchiveReader *> readers;

  for (const char *filename : filenames) {
    ArchiveReader *reader = new ArchiveReader(bmain, filename);

    if (!reader->valid()) {
      delete reader;
      continue;
    }

    readers.push_back(reader);
  }

  if (readers.empty()) {
    return nullptr;
  }

  if (readers.size() == 1) {
    return readers[0];
  }

  return new ArchiveReader(readers);
}

ArchiveReader::ArchiveReader(const std::vector<ArchiveReader *> &readers) : m_readers(readers)
{
  Alembic::AbcCoreLayer::ArchiveReaderPtrs archives;

  for (ArchiveReader *reader : readers) {
    archives.push_back(reader->m_archive.getPtr());
  }

  Alembic::AbcCoreLayer::ReadArchive layer;
  Alembic::AbcCoreAbstract::ArchiveReaderPtr arPtr = layer(archives);

  m_archive = IArchive(arPtr, kWrapExisting, ErrorHandler::kThrowPolicy);
}

ArchiveReader::ArchiveReader(const Main *bmain, const char *filename)
{
  char abs_filepath[FILE_MAX];
  STRNCPY(abs_filepath, filename);
  BLI_path_abs(abs_filepath, BKE_main_blendfile_path(bmain));

#ifdef WIN32
  UTF16_ENCODE(abs_filepath);
  std::wstring wstr(abs_filepath_16);
  m_infile.open(wstr.c_str(), std::ios::in | std::ios::binary);
  UTF16_UN_ENCODE(abs_filepath);
#else
  m_infile.open(abs_filepath, std::ios::in | std::ios::binary);
#endif

  m_streams.push_back(&m_infile);

  m_archive = open_archive(abs_filepath, m_streams);
}

ArchiveReader::~ArchiveReader()
{
  for (ArchiveReader *reader : m_readers) {
    delete reader;
  }
}

bool ArchiveReader::valid() const
{
  return m_archive.valid();
}

Alembic::Abc::IObject ArchiveReader::getTop()
{
  return m_archive.getTop();
}

bool ArchiveReader::is_blender_archive_version_prior_44()
{
  const MetaData &abc_metadata = m_archive.getPtr()->getMetaData();

  /* Was the incoming Archive written by Blender? If so, make the version check. */
  if (abc_metadata.get(Alembic::Abc::kApplicationNameKey) == "Blender") {
    return abc_metadata.get("blender_version") < "v4.4";
  }

  return false;
}

TimeInfo ArchiveReader::getTimeInfo()
{
  const uint32_t num_time_sampling_ptrs = m_archive.getNumTimeSamplings();

  chrono_t min_time = std::numeric_limits<chrono_t>::max();
  chrono_t max_time = -std::numeric_limits<chrono_t>::max();

  for (uint32_t i = 0; i < num_time_sampling_ptrs; ++i) {
    const index_t max_samples = m_archive.getMaxNumSamplesForTimeSamplingIndex(i);

    /* This can only happen in very old files, predating the original Blender Alembic support,
     * however let's make sure this case is handled. */
    if (max_samples == INDEX_UNKNOWN) {
      continue;
    }

    const TimeSamplingPtr time_sampling_ptr = m_archive.getTimeSampling(i);
    BLI_assert_msg(time_sampling_ptr,
                   "could not get Alembic time sampling even though it was declared as available");

    const TimeSamplingType &time_sampling_type = time_sampling_ptr->getTimeSamplingType();

    /* Ignore time samplings with values similar to that of a default constructed TimeSampling.
     *
     * Those are ignored as it represents a static frame of a 1 second duration which might corrupt
     * the time range computation (e.g. if the animation is supposed to be at 24 FPS, then the
     * frame duration should about 0.041 second, the default will scale up the time range by 24).
     *
     * Since time samplings are deduplicated when writing archives, if all time samplings in the
     * file have the same values as the default, then there is no animation, as there would only be
     * a single time sampling representing a static frame. The default TimeSampling should be at
     * index 0, but it is still possible that the file was badly written and duplicates exist, so
     * we perform this check in the loop.
     */
    if (time_sampling_ptr->getNumStoredTimes() == 1 &&
        time_sampling_ptr->getStoredTimes()[0] == 0.0 &&
        time_sampling_type.getTimePerCycle() == 1.0)
    {
      continue;
    }

    min_time = std::min(min_time, time_sampling_ptr->getSampleTime(0));
    max_time = std::max(max_time, time_sampling_ptr->getSampleTime(max_samples - 1));
  }

  return {min_time, max_time};
}
}  // namespace io::alembic
}  // namespace blender
