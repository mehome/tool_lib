#include "Task.h"
#include "OS.h"
#include "OSMemory.h"
#include "atomic.h"
#include "OSMutexRW.h"


unsigned int    Task::sThreadPicker = 0;
OSMutexRW       TaskThreadPool::sMutexRW;
static char* sTaskStateStr="live_"; //Alive

Task::Task()
:   fEvents(0), fUseThisThread(NULL), fWriteLock(false), fTimerHeapElem(), fTaskQueueElem()
{
#if DEBUG
    fInRunCount = 0;
#endif
    this->SetTaskName("unknown");

	fTaskQueueElem.SetEnclosingObject(this);
	fTimerHeapElem.SetEnclosingObject(this);

}

void Task::SetTaskName(char* name) 
{
    if (name == NULL) 
        return;
   
   ::strncpy(fTaskName,sTaskStateStr,sizeof(fTaskName));
   ::strncat(fTaskName,name,sizeof(fTaskName));
   fTaskName[sizeof(fTaskName) -1] = 0; //terminate in case it is longer than ftaskname.
   
}

Bool16 Task::Valid()
{
    if  (   (this->fTaskName == NULL)
         || (0 != ::strncmp(sTaskStateStr,this->fTaskName, 5))
         )
     {
        if (TASK_DEBUG) qtss_printf(" Task::Valid Found invalid task = %ld\n", this);
        
        return false;
     }
    
    return true;
}

Task::EventFlags Task::GetEvents()
{
    //Mask off every event currently in the mask except for the alive bit, of course,
    //which should remain unaffected and unreported by this call.
    EventFlags events = fEvents & kAliveOff;
    (void)atomic_sub(&fEvents, events);
    return events;
}

void Task::Signal(EventFlags events)
{
    if (!this->Valid())
        return;
        
    //Fancy no mutex implementation. We atomically mask the new events into
    //the event mask. Because atomic_or returns the old state of the mask,
    //we only schedule this task once.
    events |= kAlive;
    EventFlags oldEvents = atomic_or(&fEvents, events);
    if ((!(oldEvents & kAlive)) && (TaskThreadPool::sNumTaskThreads > 0))
    {
        if (fUseThisThread != NULL)
            // Task needs to be placed on a particular thread.
         {
            if (TASK_DEBUG) if (fTaskName[0] == 0) ::strcpy(fTaskName, " corrupt task");
            if (TASK_DEBUG) qtss_printf("Task::Signal enque TaskName=%s fUseThisThread=%lu q elem=%lu enclosing=%lu\n", fTaskName, (UInt32) fUseThisThread, (UInt32) &fTaskQueueElem, (UInt32) this);
            fUseThisThread->fTaskQueue.EnQueue(&fTaskQueueElem);
        }
        else
        {
            //find a thread to put this task on
            unsigned int theThread = atomic_add(&sThreadPicker, 1);
            theThread %= TaskThreadPool::sNumTaskThreads;
            if (TASK_DEBUG) if (fTaskName[0] == 0) ::strcpy(fTaskName, " corrupt task");
            if (TASK_DEBUG) qtss_printf("Task::Signal enque TaskName=%s thread=%lu q elem=%lu enclosing=%lu\n", fTaskName, (UInt32)TaskThreadPool::sTaskThreadArray[theThread],(UInt32) &fTaskQueueElem,(UInt32) this);
            TaskThreadPool::sTaskThreadArray[theThread]->fTaskQueue.EnQueue(&fTaskQueueElem);
        }
    }
    else
        if (TASK_DEBUG) qtss_printf("Task::Signal sent to dead TaskName=%s  q elem=%lu  enclosing=%lu\n",  fTaskName, (UInt32) &fTaskQueueElem, (UInt32) this);
        

}


void    Task::GlobalUnlock()    
{   
    if (this->fWriteLock)
    {   this->fWriteLock = false;   
        TaskThreadPool::sMutexRW.Unlock();
    }                                               
}



void TaskThread::Entry()
{
    Task* theTask = NULL;
    
    while (true) 
    {
        theTask = this->WaitForTask();

        //
        // WaitForTask returns NULL when it is time to quit
        if (theTask == NULL || false == theTask->Valid() )
            return;
                    
        Bool16 doneProcessingEvent = false;
        
        while (!doneProcessingEvent)
        {
            //If a task holds locks when it returns from its Run function,
            //that would be catastrophic and certainly lead to a deadlock
#if DEBUG
            Assert(this->GetNumLocksHeld() == 0);
            Assert(theTask->fInRunCount == 0);
            theTask->fInRunCount++;
#endif
            theTask->fUseThisThread = NULL; // Each invocation of Run must independently
                                            // request a specific thread.
            SInt64 theTimeout = 0;
            
            if (theTask->fWriteLock)
            {   
                OSMutexWriteLocker mutexLocker(&TaskThreadPool::sMutexRW);
                if (TASK_DEBUG) qtss_printf("TaskThread::Entry run global locked TaskName=%s CurMSec=%.3f thread=%ld task=%ld\n", theTask->fTaskName, OS::StartTimeMilli_Float() ,(SInt32) this,(SInt32) theTask);
                
                theTimeout = theTask->Run();
                theTask->fWriteLock = false;
            }
            else
            {
                OSMutexReadLocker mutexLocker(&TaskThreadPool::sMutexRW); /*?????????*/
                if (TASK_DEBUG) qtss_printf("TaskThread::Entry run TaskName=%s CurMSec=%.3f thread=%ld task=%ld\n", theTask->fTaskName, OS::StartTimeMilli_Float(), (SInt32) this,(SInt32) theTask);

                theTimeout = theTask->Run();
            
            }
#if DEBUG
            Assert(this->GetNumLocksHeld() == 0);
            theTask->fInRunCount--;
            Assert(theTask->fInRunCount == 0);
#endif          
            if (theTimeout < 0)
            {
                if (TASK_DEBUG) 
                {
                    qtss_printf("TaskThread::Entry delete TaskName=%s CurMSec=%.3f thread=%ld task=%ld\n", theTask->fTaskName, OS::StartTimeMilli_Float(), (SInt32) this, (SInt32) theTask);
                     
                    theTask->fUseThisThread = NULL;
                    
                    if (NULL != fHeap.Remove(&theTask->fTimerHeapElem)) 
                        qtss_printf("TaskThread::Entry task still in heap before delete\n");
                    
                    if (NULL != theTask->fTaskQueueElem.InQueue())
                        qtss_printf("TaskThread::Entry task still in queue before delete\n");
                    
                    theTask->fTaskQueueElem.Remove();
                    
                    if (theTask->fEvents &~ Task::kAlive)
                        qtss_printf ("TaskThread::Entry flags still set  before delete\n");

                    (void)atomic_sub(&theTask->fEvents, 0);
                     
                    ::strncat (theTask->fTaskName, " deleted", sizeof(theTask->fTaskName) -1);
                }
                theTask->fTaskName[0] = 'D'; //mark as dead
                delete theTask;
                theTask = NULL;
                doneProcessingEvent = true;

            }
            else if (theTimeout == 0)
            {
                //We want to make sure that 100% definitely the task's Run function WILL
                //be invoked when another thread calls Signal. We also want to make sure
                //that if an event sneaks in right as the task is returning from Run()
                //(via Signal) that the Run function will be invoked again.
                doneProcessingEvent = compare_and_store(Task::kAlive, 0, &theTask->fEvents);
                if (doneProcessingEvent)
                    theTask = NULL; 
            }
            else
            {
                //note that if we get here, we don't reset theTask, so it will get passed into
                //WaitForTask
                if (TASK_DEBUG) qtss_printf("TaskThread::Entry insert TaskName=%s in timer heap thread=%lu elem=%lu task=%ld timeout=%.2f\n", theTask->fTaskName,  (UInt32) this, (UInt32) &theTask->fTimerHeapElem,(SInt32) theTask, (float)theTimeout / (float) 1000);
                theTask->fTimerHeapElem.SetValue(OS::Milliseconds() + theTimeout);
                fHeap.Insert(&theTask->fTimerHeapElem);
                (void)atomic_or(&theTask->fEvents, Task::kIdleEvent);
                doneProcessingEvent = true;
            }
        
        
        #if TASK_DEBUG
        SInt64  yieldStart = OS::Milliseconds();
        #endif
        
        this->ThreadYield();
        #if TASK_DEBUG
        SInt64  yieldDur = OS::Milliseconds() - yieldStart;
        static SInt64   numZeroYields;
        
        if ( yieldDur > 1 )
        {
            if (TASK_DEBUG) qtss_printf( "TaskThread::Entry time in Yield %i, numZeroYields %i\n", (long)yieldDur, (long)numZeroYields );
            numZeroYields = 0;
        }
        else
            numZeroYields++;
        #endif
        
        }
    }
}

Task* TaskThread::WaitForTask()
{
    while (true)
    {
        SInt64 theCurrentTime = OS::Milliseconds();
        
        if ((fHeap.PeekMin() != NULL) && (fHeap.PeekMin()->GetValue() <= theCurrentTime))
        {    
            if (TASK_DEBUG) qtss_printf("TaskThread::WaitForTask found timer-task=%s thread %lu fHeap.CurrentHeapSize(%lu) taskElem = %lu enclose=%lu\n",((Task*)fHeap.PeekMin()->GetEnclosingObject())->fTaskName, (UInt32) this, fHeap.CurrentHeapSize(), (UInt32) fHeap.PeekMin(), (UInt32) fHeap.PeekMin()->GetEnclosingObject());
            return (Task*)fHeap.ExtractMin()->GetEnclosingObject();
        }
    
        //if there is an element waiting for a timeout, figure out how long we should wait.
        SInt64 theTimeout = 0;
        if (fHeap.PeekMin() != NULL)
            theTimeout = fHeap.PeekMin()->GetValue() - theCurrentTime;
        Assert(theTimeout >= 0);
        
        //
        // Make sure we can't go to sleep for some ridiculously short
        // period of time
        // Do not allow a timeout below 10 ms without first verifying reliable udp 1-2mbit live streams. 
        // Test with streamingserver.xml pref reliablUDP printfs enabled and look for packet loss and check client for  buffer ahead recovery.
	if (theTimeout < 10) 
           theTimeout = 10;
            
        //wait...
        OSQueueElem* theElem = fTaskQueue.DeQueueBlocking(this, (SInt32) theTimeout);
        fprintf(stderr,"Task Queue Ele Numbers %u\n",fTaskQueue.GetLength());
        if (theElem != NULL)
        {    
            if (TASK_DEBUG) qtss_printf("TaskThread::WaitForTask found signal-task=%s thread %lu fTaskQueue.GetLength(%lu) taskElem = %lu enclose=%lu\n", ((Task*)theElem->GetEnclosingObject())->fTaskName,  (UInt32) this, fTaskQueue.GetQueue()->GetLength(), (UInt32)  theElem,  (UInt32)theElem->GetEnclosingObject() );
            return (Task*)theElem->GetEnclosingObject();
        }

        //
        // If we are supposed to stop, return NULL, which signals the caller to stop
        if (OSThread::GetCurrent()->IsStopRequested())
            return NULL;
    }   
}

TaskThread** TaskThreadPool::sTaskThreadArray = NULL;
UInt32       TaskThreadPool::sNumTaskThreads = 0;

Bool16 TaskThreadPool::AddThreads(UInt32 numToAdd)
{
    Assert(sTaskThreadArray == NULL);
    sTaskThreadArray = new TaskThread*[numToAdd];
        
    for (UInt32 x = 0; x < numToAdd; x++)
    {
        sTaskThreadArray[x] = NEW TaskThread();
        sTaskThreadArray[x]->Start();
    }
    sNumTaskThreads = numToAdd;
    return true;
}


void TaskThreadPool::RemoveThreads()
{
    //Tell all the threads to stop
    for (UInt32 x = 0; x < sNumTaskThreads; x++)
        sTaskThreadArray[x]->SendStopRequest();

    //Because any (or all) threads may be blocked on the queue, cycle through
    //all the threads, signalling each one
    for (UInt32 y = 0; y < sNumTaskThreads; y++)
        sTaskThreadArray[y]->fTaskQueue.GetCond()->Signal();
    
    //Ok, now wait for the selected threads to terminate, deleting them and removing
    //them from the queue.
    for (UInt32 z = 0; z < sNumTaskThreads; z++)
        delete sTaskThreadArray[z];
    
    sNumTaskThreads = 0;
}
