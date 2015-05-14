#include "VirtualMachine.h"
#include "UtilityFunctions.h"
#include "Machine.h"
#include <stdio.h>
#include <math.h>

TVMThreadPriority defaultSchedulerPriority = VM_THREAD_PRIORITY_HIGH;
TCB* vmStartThread;

extern "C" {



//=== Memory Pools ==================================================================

    TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer)
    {
        ThreadStore* tStore = ThreadStore::getInstance();
        MemoryPool *memoryPool = tStore->findMemoryPoolByID(memory);
        if ((memory == NULL) || (size = 0) || (pointer == NULL)){
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else if (size > memoryPool->getSize()){
            return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
        }
        else
            valloc((int)ceil(size/64));
            return VM_STATUS_SUCCESS;
    }

    
    TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory)
    {
        if((base == NULL) || (memory == NULL) || (size = 0)){
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        MemoryPool* memoryPool = new MemoryPool();
        *memory = memoryPool->getID();
        return VM_STATUS_SUCCESS;
    }

    
    TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory)
    {
        if (memory == NULL){
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        return VM_STATUS_ERROR_INVALID_STATE;
        return VM_STATUS_SUCCESS;
    }
    
    TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft)
    {
        if ((memory == NULL) || (bytesleft == NULL)){
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        return VM_STATUS_SUCCESS;
    }
    
    TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer)
    {
        if ((memory == NULL) || (pointer == NULL)){
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (!pointer){
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else
            VM_STATUS_ERROR_INVALID_PARAMETER;
    }

    
//===================================================================================


//=== Mutex ==================================================================


TVMStatus VMMutexCreate(TVMMutexIDRef mutexref){
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	
	if(mutexref == NULL){
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	Mutex* mutex = new Mutex();
	tStore->insert(mutex);	//insert is overloaded to work with mutexes and TCB*s
	*mutexref = mutex->getID();
	
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutexID){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	Mutex* mutex = tStore->findMutexByID(mutexID);	

	if(mutex == NULL){	//if the mutex does not exist
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}

	if(mutex->isDeleted() == true){			//mutex is marked for deletion
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}

	if(mutex->getOwner() != NULL){					//if the mutex is held, do not delete it
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_STATE;
	}

	//else, mark for deletion
	mutex->deleteFromVM();
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutexID, TVMThreadIDRef ownerref){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	Mutex* mutex = tStore->findMutexByID(mutexID);
	
	if(ownerref == NULL){
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	if(mutex == NULL){
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}
	
	if(mutex->isDeleted() == true){
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}

	if(mutex->getOwner() == NULL){		//if the mutex is UNLOCKED, error
		MachineResumeSignals(&sigState);
  	return VM_THREAD_ID_INVALID;
	}

	//if the mutex exists, is not deleted, is not free, and if we have somewhere to put the result...	
	*ownerref = mutex->getOwner()->getThreadID();	//return the owner
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutexID, TVMTick timeout){
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();
	Mutex* mutex = tStore->findMutexByID(mutexID);
	
	if(mutex == NULL){	//if mutex doesnt exist, error
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}

	if(mutex->isDeleted() == true){	//if mutex deleted, error
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}

	mutex->lock(currentThread, timeout);	//may block here until timeout expires

	if(mutex->getOwner() != currentThread){
		MachineResumeSignals(&sigState);
		return VM_STATUS_FAILURE;
	}

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexRelease(TVMMutexID mutexID){
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();
	Mutex* mutex = tStore->findMutexByID(mutexID);
	
	if(mutex == NULL){
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_ID;
	}

	if(mutex->getOwner() != currentThread){
		return VM_STATUS_ERROR_INVALID_STATE;
	}

	mutex->release();
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

//============================================================================

//=== Files and IO ==================================================================

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();
	
	if(newoffset == NULL){
		MachineResumeSignals(&sigState);
  	return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MachineFileSeek(filedescriptor, offset, whence, &machineFileIOCallback, (void*)currentThread);
	tStore->waitCurrentThreadOnIO();//resume execution here after IO completes
	*newoffset = currentThread->getFileIOResult();	
	
	if(currentThread->getFileIOResult() < 0){	//if call to open() failed
		MachineResumeSignals(&sigState);
		return VM_STATUS_FAILURE;		//return failure state
	}
	
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();
  
	if ((data == NULL) || (length == NULL)){
		MachineResumeSignals (&sigState);
  	return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MachineFileRead(filedescriptor, data, *length, &machineFileIOCallback, (void*)currentThread);
	tStore->waitCurrentThreadOnIO();								//resume execution here after IO completes
	*length = currentThread->getFileIOResult();
	
	if(currentThread->getFileIOResult() < 0){			//if call to open() failed
		MachineResumeSignals (&sigState);
		return VM_STATUS_FAILURE;											//return failure state
	}

	MachineResumeSignals (&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileClose(int filedescriptor){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();

	MachineFileClose(filedescriptor, machineFileIOCallback, (void*)currentThread);
	tStore->waitCurrentThreadOnIO();//resume execution here after IO completes
	
	if(currentThread->getFileIOResult() < 0){
		MachineResumeSignals (&sigState);
    return VM_STATUS_FAILURE;
	}
	
	MachineResumeSignals (&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();
  
	if ((filename == NULL) || (filedescriptor == NULL)){
		MachineResumeSignals (&sigState);
  	return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MachineFileOpen(filename, flags, mode, &machineFileIOCallback, (void*)currentThread);
	tStore->waitCurrentThreadOnIO();//resume execution here after IO completes
	*filedescriptor = currentThread->getFileIOResult();	//return result by reference
	
	if(currentThread->getFileIOResult() < 0){	//if call to open() failed
		MachineResumeSignals (&sigState);
		return VM_STATUS_FAILURE;		//return failure state
	}
	
	MachineResumeSignals (&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int fileDescriptor, void* data, int *length){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();
	
	if((data == NULL) || (length == NULL)){
		MachineResumeSignals (&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MachineFileWrite(fileDescriptor, data, *length, &machineFileIOCallback, (void*)currentThread);
	//This works because idle() has a call to MachineEnableSignals in it. Otherwise would need MachineEnableSignals() here	
	tStore->waitCurrentThreadOnIO();	//switch to a new thread here, don't do a wait loop
	*length = currentThread->getFileIOResult();

	if(currentThread->getFileIOResult() < 0){
		MachineResumeSignals (&sigState);
		return VM_STATUS_FAILURE;
	}
	MachineResumeSignals (&sigState);
	return VM_STATUS_SUCCESS;
}

//==================================================================================



//=== Threads ======================================================================
//

TVMStatus VMThreadDelete(TVMThreadID thread){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->findThreadByID(thread);	//returns NULL on not finding thread

	if(currentThread == NULL){								//if thread was not found, error
		MachineResumeSignals (&sigState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	
	if(currentThread->getState() != VM_THREAD_STATE_DEAD){
		MachineResumeSignals (&sigState);
		return VM_STATUS_ERROR_INVALID_STATE;
	}

	tStore->deleteDeadThread(currentThread);
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadTerminate(TVMThreadID thread){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* threadToTerminate = tStore->findThreadByID(thread);	//returns NULL on not finding thread


	if(threadToTerminate == NULL){								//if thread was not found, error
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	
	if(threadToTerminate->getState() == VM_THREAD_STATE_DEAD){
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_STATE;
	}
	else{
		tStore->terminate(threadToTerminate);
		MachineResumeSignals(&sigState);
		return VM_STATUS_SUCCESS;
	}
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateRef){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);

	if(stateRef == NULL){											//if stateRef is null, error
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->findThreadByID(thread);	//returns NULL on not finding thread

	if(currentThread == NULL){								//if thread was not found, error
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_ID;
	}
	*stateRef = currentThread->getState();		//otherwise, success
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMThreadActivate(TVMThreadID thread){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	TCB* threadToActivate = tStore->findThreadByID(thread);

	if(threadToActivate == NULL){					//thread ID does not exist
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_ID;	//error status
	}
	
	if(threadToActivate->getState() != VM_THREAD_STATE_DEAD){	//thread exists but in wrong state
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_STATE;										//error status
	}
	tStore->activateDeadThread(threadToActivate);	//activate given thread from dead list	
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;							//only other alternative is success
}

TVMStatus VMThreadID(TVMThreadIDRef threadRef){
//returns thread id of currently running thread into location specified by threadref
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	
	if(threadRef == NULL){
			MachineResumeSignals(&sigState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		//need to SET the active thread before calling this!
		TCB* currentThread = ThreadStore::getInstance()->getCurrentThread();
		TVMThreadID currentThreadID = currentThread->getThreadID();
		*threadRef = currentThreadID;
		MachineResumeSignals(&sigState);
		return VM_STATUS_SUCCESS;			//thread ID is successfully retrieved, stored in the location specified by threadref,
	}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid){

		TMachineSignalState sigState;
		MachineSuspendSignals(&sigState);
		
		if((entry == NULL) || (tid == NULL)){
			MachineResumeSignals(&sigState);
			return VM_STATUS_ERROR_INVALID_PARAMETER;
		}

		ThreadStore *tStore = ThreadStore::getInstance(); 					//get the threadstore
		TCB* tcb = new TCB(entry, param, memsize, prio, tid);				//create TCB 
		
		tStore->insert(tcb);																				//pushes TCB into appropriate queue in ThreadStore
		MachineResumeSignals(&sigState);
		return VM_STATUS_SUCCESS;
	}

TVMStatus VMThreadSleep(TVMTick tick){
//Puts running thread to sleep for tick ticks/quantums
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);
	ThreadStore *tStore = ThreadStore::getInstance();

	if(tick == VM_TIMEOUT_INFINITE){
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	if(tick == VM_TIMEOUT_IMMEDIATE){
		tStore->sleepCurrentThread(0);	//sets the current thread to sleep and schedules a new one in its place
	}
	else{
		tStore->sleepCurrentThread(tick);
	}//both cases sleep the running process and immediately call the scheduler to swap a new process in
	
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, TVMMemorySize sharedsize, int argc, char* argv[]){

	//tickms is the time in milliseconds for the alarm, this will be the quantum. 
	//machineticksms is the tick time of the machine, how long it will sleep in between actions.

	void* sharedMemoryBaseAddress = NULL;	
	const char* module = (const char*) argv[0];	//get the module from the command-line args
	sharedMemoryBaseAddress = MachineInitialize(machinetickms, (size_t)sharedsize);	

	ThreadStore* tStore = ThreadStore::getInstance();
	tStore->setSharedMemoryParams(sharedMemoryBaseAddress, heapsize);
	tStore->createMainThread();	//mainThread is ID 1
	tStore->createIdleThread();	//idleThread is ID 2

	//request alarm every tickms USECONDS. alarm callback is schedule(high priority)
	MachineEnableSignals();	//enable signals	
	MachineRequestAlarm(1000*tickms, (TMachineAlarmCallback)&timerInterrupt, (void*)&defaultSchedulerPriority);	
	TVMMainEntry VMMain = VMLoadModule(module);							//load the module, save the VMMain function reference
	
	if(VMMain != NULL){													//if VMMain is a valid reference,
		VMMain(argc, argv);												//run the loaded module,
		VMUnloadModule();													//then unload the module
		delete tStore;														//delete unnecessary container
		MachineTerminate();
		return VM_STATUS_SUCCESS;
	}
	else{																				//if VMMain is null, the module failed to load
		VMUnloadModule();													//unload the module and return error status
		MachineTerminate();
		return VM_STATUS_FAILURE;
	}
}
}
