/*
 * NotebookPlots.cpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionRmdNotebook.hpp"
#include "NotebookPlots.hpp"
#include "NotebookOutput.hpp"
#include "../SessionPlots.hpp"

#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/signals/connection.hpp>

#include <core/system/FileMonitor.hpp>
#include <core/StringUtils.hpp>
#include <core/Exec.hpp>

#include <session/SessionModuleContext.hpp>

#include <r/RExec.hpp>
#include <r/RSexp.hpp>
#include <r/session/RGraphics.hpp>
#include <r/ROptions.hpp>

#define kPlotPrefix "_rs_chunk_plot_"
#define kGoldenRatio 1.618

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {
namespace {

bool isPlotPath(const FilePath& path)
{
   return path.hasExtensionLowerCase(".png") &&
          string_utils::isPrefixOf(path.stem(), kPlotPrefix);
}

} // anonymous namespace

PlotCapture::PlotCapture() :
   hasPlots_(false),
   plotPending_(false),
   lastOrdinal_(0)
{
}

PlotCapture::~PlotCapture()
{
}

void PlotCapture::processPlots(bool ignoreEmpty)
{
   // ensure plot folder exists
   if (!plotFolder_.exists())
      return;

   // collect plots from the folder
   std::vector<FilePath> folderContents;
   Error error = plotFolder_.children(&folderContents);
   if (error)
      LOG_ERROR(error);
   
   BOOST_FOREACH(const FilePath& path, folderContents)
   {
      if (isPlotPath(path))
      {
         // we might find an empty plot file if it hasn't been flushed to disk
         // yet--ignore these
         if (ignoreEmpty && path.size() == 0)
            continue;

         // emit the plot and the snapshot file
         events().onPlotOutput(path, snapshotFile_, lastOrdinal_);

         // we've consumed the snapshot file, so clear it
         snapshotFile_ = FilePath();
         lastOrdinal_ = 0;

         // clean up the plot so it isn't emitted twice
         error = path.removeIfExists();
         if (error)
            LOG_ERROR(error);
      }
   }
}

void PlotCapture::saveSnapshot()
{
   // no work to do if we don't have a display list to write
   if (lastPlot_.isNil())
      return;

   // if there's a plot on the device, write its display list before it's
   // cleared for the next page
   FilePath outputFile = plotFolder_.complete(
         core::system::generateUuid(false) + kDisplayListExt);

   Error error = r::exec::RFunction(".rs.saveNotebookGraphics", 
         lastPlot_.get(), outputFile.absolutePath()).call();
   if (error)
      LOG_ERROR(error);
   else
      snapshotFile_ = outputFile;
}

void PlotCapture::onExprComplete()
{
   r::sexp::Protect protect;

   // no action if no plots were created in this chunk
   if (!hasPlots_)
      return;

   // no action if nothing on device list (implies no graphics output)
   if (!isGraphicsDeviceActive())
      return;
   
   // if we were expecting a new plot to be produced by the previous
   // expression, process the plot folder
   if (plotPending_)
   {
      plotPending_ = false;
      processPlots(true);
   }

   // check the current state of the graphics device against the last known
   // state
   SEXP plot = R_NilValue;
   Error error = r::exec::RFunction("recordPlot").call(&plot, &protect);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   // detect changes and save last state
   bool unchanged = false;
   if (!lastPlot_.isNil())
      r::exec::RFunction("identical", plot, lastPlot_.get()).call(&unchanged);
   lastPlot_.set(plot);

   // if the state changed, reserve an ordinal at this position
   if (!unchanged)
   {
      OutputPair pair = lastChunkOutput(docId_, chunkId_, nbCtxId_);
      lastOrdinal_ = ++pair.ordinal;
      pair.outputType = ChunkOutputPlot;
      updateLastChunkOutput(docId_, chunkId_, pair);

      // notify the client so it can create a placeholder
      json::Object unit;
      unit[kChunkOutputType]    = static_cast<int>(ChunkOutputOrdinal);
      unit[kChunkOutputValue]   = static_cast<int>(lastOrdinal_);
      unit[kChunkOutputOrdinal] = static_cast<int>(lastOrdinal_);
      json::Object placeholder;
      placeholder[kChunkId]         = chunkId_;
      placeholder[kChunkDocId]      = docId_;
      placeholder[kChunkOutputPath] = unit;

      module_context::enqueClientEvent(ClientEvent(
               client_events::kChunkOutput, placeholder));
   }

}

void PlotCapture::removeGraphicsDevice()
{
   // take a snapshot of the last plot's display list before we turn off the
   // device (if we haven't emitted it yet)
   if (hasPlots_ && 
       sizeBehavior_ == PlotSizeAutomatic &&
       snapshotFile_.empty())
      saveSnapshot();

   // turn off the graphics device, if it was ever turned on -- this has the
   // side effect of writing the device's remaining output to files
   if (isGraphicsDeviceActive())
   {
      Error error = r::exec::RFunction("dev.off").call();
      if (error)
         LOG_ERROR(error);

      // some operations may trigger the graphics device without actually
      // writing a plot; ignore these
      if (hasPlots_)
         processPlots(false);
   }
   hasPlots_ = false;
}

void PlotCapture::onBeforeNewPlot()
{
   if (!lastPlot_.isNil())
   {
      // save the snapshot of the plot to disk
      if (sizeBehavior_ == PlotSizeAutomatic)
         saveSnapshot();
   }
   plotPending_ = true;
   hasPlots_ = true;
}

void PlotCapture::onNewPlot()
{
   hasPlots_ = true;
   processPlots(true);
}

// begins capturing plot output
core::Error PlotCapture::connectPlots(const std::string& docId, 
      const std::string& chunkId, const std::string& nbCtxId, 
      double height, double width, PlotSizeBehavior sizeBehavior,
      const FilePath& plotFolder)
{
   // save identifiers
   docId_ = docId;
   chunkId_ = chunkId;
   nbCtxId_ = nbCtxId;

   // clean up any stale plots from the folder
   plotFolder_ = plotFolder;
   std::vector<FilePath> folderContents;
   Error error = plotFolder.children(&folderContents);
   if (error)
      return error;

   BOOST_FOREACH(const core::FilePath& file, folderContents)
   {
      // remove if it looks like a plot 
      if (isPlotPath(file)) 
      {
         error = file.remove();
         if (error)
         {
            // this is non-fatal 
            LOG_ERROR(error);
         }
      }
   }

   // infer height/width if only one is given
   if (height == 0 && width > 0)
      height = width / kGoldenRatio;
   else if (height > 0 && width == 0)
      width = height * kGoldenRatio;
   width_ = width;
   height_ = height;
   sizeBehavior_ = sizeBehavior;

   // save old device option
   deviceOption_.set(r::options::getOption("device"));

   // set option for notebook graphics device (must succeed)
   error = setGraphicsOption();
   if (error)
      return error;

   onBeforeNewPlot_ = plots::events().onBeforeNewPlot.connect(
         boost::bind(&PlotCapture::onBeforeNewPlot, this));
   
   onBeforeNewGridPage_ = plots::events().onBeforeNewGridPage.connect(
         boost::bind(&PlotCapture::onBeforeNewPlot, this));

   onNewPlot_ = plots::events().onNewPlot.connect(
         boost::bind(&PlotCapture::onNewPlot, this));

   NotebookCapture::connect();
   return Success();
}

void PlotCapture::disconnect()
{
   if (connected())
   {
      // remove the graphics device if we created it
      removeGraphicsDevice();

      // restore the graphics device option
      r::options::setOption("device", deviceOption_.get());

      onNewPlot_.disconnect();
      onBeforeNewPlot_.disconnect();
      onBeforeNewGridPage_.disconnect();
   }
   NotebookCapture::disconnect();
}

core::Error PlotCapture::setGraphicsOption()
{
   Error error;

   // create the notebook graphics device
   r::exec::RFunction setOption(".rs.setNotebookGraphicsOption");

   // the folder in which to place the rendered plots (this is a sibling of the
   // main chunk output folder)
   setOption.addParam(
         plotFolder_.absolutePath() + "/" kPlotPrefix "%03d.png");

   // device dimensions
   setOption.addParam(height_);
   setOption.addParam(width_); 

   // sizing behavior drives units -- user specified units are in inches but
   // we use pixels when scaling automatically
   setOption.addParam(sizeBehavior_ == PlotSizeManual ? "in" : "px");

   // device parameters
   setOption.addParam(r::session::graphics::device::devicePixelRatio());

   // other args (OS dependent)
   setOption.addParam(r::session::graphics::extraBitmapParams());

   return setOption.call();
}

bool PlotCapture::isGraphicsDeviceActive()
{
   r::sexp::Protect protect;
   SEXP devlist = R_NilValue;
   Error error = r::exec::RFunction("dev.list").call(&devlist, &protect);
   if (error)
      LOG_ERROR(error);
   if (r::sexp::isNull(devlist))
      return false;
   return true;
}

core::Error initPlots()
{
   ExecBlock initBlock;
   initBlock.addFunctions()
      (boost::bind(module_context::sourceModuleRFile, "NotebookPlots.R"));

   return initBlock.execute();
}

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio

