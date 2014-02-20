/*************************************************************************
 * Copyright 2009-2014 Eucalyptus Systems, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 *
 * Please contact Eucalyptus Systems, Inc., 6755 Hollister Ave., Goleta
 * CA 93117, USA or visit http://www.eucalyptus.com/licenses/ if you need
 * additional information or have any questions.
 ************************************************************************/
package com.eucalyptus.imaging;

import org.apache.log4j.Logger;

import com.eucalyptus.imaging.AbstractTaskScheduler.WorkerTask;
import com.eucalyptus.util.EucalyptusCloudException;
public class ImagingService {
  private static Logger LOG = Logger.getLogger( ImagingService.class );

  public PutInstanceImportTaskStatusResponseType PutInstanceImportTaskStatus( PutInstanceImportTaskStatusType request ) throws EucalyptusCloudException {
    final PutInstanceImportTaskStatusResponseType reply = request.getReply( );
    reply.setCancelled(false);

    try{
      final String taskId = request.getImportTaskId();
      ImagingTask imagingTask = null;
      
      try{
        imagingTask= ImagingTasks.lookup(taskId);
      }catch(final Exception ex){
        LOG.warn("imaging task with "+taskId+" is not found");
        reply.setCancelled(true);
      }
      if(imagingTask!=null){
        //EXTANT, FAILED, DONE
        final WorkerTaskState workerState = WorkerTaskState.fromString(request.getStatus());
        switch(workerState){
        case EXTANT:
          ;
          break;

        case DONE:
          ImagingTasks.setState(imagingTask, ImportTaskState.COMPLETED, null);
          break;

        case FAILED:
          ImagingTasks.setState(imagingTask, ImportTaskState.FAILED, request.getStatusMessage());
          break;
        }
      }
    }catch(final Exception ex){
      LOG.error("Failed to update the task's state", ex);
    }
    return reply;
  }

  public GetInstanceImportTaskResponseType GetInstanceImportTask( GetInstanceImportTaskType request ) throws EucalyptusCloudException {
    final GetInstanceImportTaskResponseType reply = request.getReply( );
    LOG.debug(request);
    try{
      final WorkerTask task = AbstractTaskScheduler.getScheduler().getTask();
      // there is a chance that there is no task to serve
      if (task != null) {
        reply.setImportTaskId(task.getTaskId());
        reply.setManifestUrl(task.getDownloadManifestUrl());
        reply.setVolumeId(task.getVolumeId());
      } else {
        reply.setImportTaskId(null);
      }
      
    }catch(final Exception ex){
      LOG.error("Failed to schedule a task", ex);
    }
    LOG.debug(reply);
    return reply;
  }
}
