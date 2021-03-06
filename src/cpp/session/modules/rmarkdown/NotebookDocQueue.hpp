/*
 * NotebookDocQueue.hpp
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

#ifndef SESSION_NOTEBOOK_DOC_QUEUE_HPP
#define SESSION_NOTEBOOK_DOC_QUEUE_HPP

#include "NotebookQueue.hpp"
#include "NotebookQueueUnit.hpp"
#include "SessionRmdNotebook.hpp"

#include <core/json/Json.hpp>
#include <core/FilePath.hpp>
#include <list>

namespace rstudio {

namespace core {
   class Error;
}

namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

class NotebookDocQueue : boost::noncopyable
{
public:
   NotebookDocQueue(const std::string& docId, const std::string& jobDesc,
         CommitMode commitMode, int pixelWith, int charWidth, int maxUnits);

   static core::Error fromJson(const core::json::Object& source,
      boost::shared_ptr<NotebookDocQueue>* pQueue);
   core::json::Object toJson() const;

   core::Error update(const boost::shared_ptr<NotebookQueueUnit> unit,
      QueueOperation op, const std::string& before);
   boost::shared_ptr<NotebookQueueUnit> firstUnit();

   core::json::Object defaultChunkOptions() const;
   void setDefaultChunkOptions(const core::json::Object& options);

   // accessors
   std::string docId() const;
   int pixelWidth() const;
   int charWidth() const;
   bool complete() const;
   CommitMode commitMode() const;

private:
   std::string docId_;
   std::string jobDesc_;
   CommitMode commitMode_;
   int pixelWidth_;
   int charWidth_;
   int maxUnits_;

   // the document path and its default knit chunk options
   std::string docPath_;
   core::json::Object defaultOptions_;

   // the queue of chunks to be executed 
   std::list<boost::shared_ptr<NotebookQueueUnit> > queue_;
};

} // namespace notebook
} // namespace rmarkdown
} // namespace modules
} // namespace session
} // namespace rstudio

#endif
