#include "VirtualMachine.h"
#include "UtilityFunctions.h"
#include "Machine.h"
#include <stdio.h>
#include <cstring>

const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;
TVMThreadPriority defaultSchedulerPriority = VM_THREAD_PRIORITY_HIGH;
TCB* vmStartThread;

extern "C" {



//=== Memory Pools ==================================================================

TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory){
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();

	if((base == NULL) || (memory == NULL)){
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	MemoryPool *pool = new MemoryPool((uint8_t*)base, size);
	*memory = pool->getID();
	tStore->insert(pool);
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer){
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();

	if((size == 0) || (pointer == NULL)){
		printf("VMMemoryPoolAllocate(): size was zero or pointer was null.\n");
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

//	printf("VMMemoryPoolAllocate(): looking for pool: %d\n", memory);
	MemoryPool *pool = tStore->findMemoryPoolByID(memory);

	if(pool == NULL){
		printf("VMMemoryPoolAllocate(): pool was null.\n");
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	uint8_t* newMemory = pool->allocateMemory(size);
//	printf("VMMemoryPoolAllocate(): allocated chunk %d\n", newMemory);

	if(newMemory == NULL){
		printf("VMMemoryPoolAllocate(): new memory allocated was null.\n");
		return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
	}

//	printf("VMMemoryPoolAllocate(): Memory allocated successfully!\n");
	*pointer = newMemory;										//if execution gets here, everything is valid and
	MachineResumeSignals(&sigState);				//the memory should be allocated
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer){
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();

	if(pointer == NULL){
		printf("VMMemoryPoolDeallocate(): pointer was null.\n");
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	MemoryPool *pool = tStore->findMemoryPoolByID(memory);
	
	if(pool == NULL){
		printf("VMMemoryPoolDeallocate(): pool was null.\n");
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

//	printf("VMMemoryPoolDeallocate(): attempting to deallocate chunk %d\n", pointer);

	if(pool->deallocate((uint8_t*)pointer) == false){	//attempts to deallocate the memory specified by pointer
		return VM_STATUS_ERROR_INVALID_PARAMETER;	//returns true on successful deallocation, and false if the
	}//memory pointed to by pointer was not previously allocated in the memory pool

	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory){
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();
	MemoryPool *pool = tStore->findMemoryPoolByID(memory);
	
//	printf("Attempting to delete memory pool MID: %d\n", memory);

	if(pool == NULL){
		printf("VMMemoryPoolDelete(): pool was null.\n");
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	if(pool->isInUse()){	//returns TRUE if any memory is allocated in the pool
		printf("VMMemoryPoolDelete(): pool still has memory allocated, so it cannot be deleted.\n");
		return VM_STATUS_ERROR_INVALID_STATE;	
	}

//	printf("VMMemoryPoolDelete(): pool deleted successfully!\n");
	tStore->deleteMemoryPool(memory);
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesLeft){
	TMachineSignalState sigState;	
	MachineSuspendSignals(&sigState);
	ThreadStore* tStore = ThreadStore::getInstance();

	if(bytesLeft == NULL){
		printf("VMMemoryPoolQuery(): bytesLeft is null\n");
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	MemoryPool *pool = tStore->findMemoryPoolByID(memory);

	if(pool == NULL){	//the memory pool was not found in the system
		printf("VMMemoryPoolQuery(): pool MID %d could not be found\n", memory);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	*bytesLeft = pool->getNumberOfUnallocatedBytes();
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
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

TVMStatus VMFileRead(int filedescriptor, void *data, int *length){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	MemoryPool* sharedMemory = tStore->findMemoryPoolByID((TVMMemoryPoolID)1);
	TCB* currentThread = tStore->getCurrentThread();
	uint8_t* sharedLocation;
	TVMMemorySize allocSize = *length;

	//Step 0 - validation
	if ((data == NULL) || (length == NULL)){
		MachineResumeSignals (&sigState);
  	return VM_STATUS_ERROR_INVALID_PARAMETER;
	}

	//Step 1 - initialize shared memory location, and wait if memory is not available from share pool
	if(allocSize > 512){	//make sure thread never asks for > 512 bytes
		allocSize = 512;
	}

	if(allocSize > sharedMemory->getSize()){
		allocSize = sharedMemory->getSize();
	}

	sharedLocation = sharedMemory->allocateMemory(allocSize);		//allocate a space in the shared memory pool
	
	if(sharedLocation == NULL){
		//if there isn't enough room in shared memory to do the IO, the current thread must wait until a chunk
		//of size allocSize becomes avaliable. waitCurrentThread puts the thread to sleep until that happens,
		//then returns the amount of shared memory requested.
		sharedLocation = tStore->waitCurrentThreadOnMemory(allocSize, 1);
	}
	
	//Step 2 - Set up loop 
	int bytesLeft = *length;
	int chunkSize = bytesLeft;
	void *dataLocation = data;
	int bytesTransferred = 0;
	
	if(bytesLeft > allocSize)
		chunkSize = allocSize;

	for(bytesLeft; bytesLeft > 0; bytesLeft -= chunkSize){

		if(bytesLeft < chunkSize)
			chunkSize = bytesLeft;

		//read the amount of data specified by chunkSize into the location specified by sharedLocation
		MachineFileRead(filedescriptor, (void*)sharedLocation, chunkSize, &machineFileIOCallback, (void*)currentThread);
		tStore->waitCurrentThreadOnIO();															//resume execution here after IO completes
		//copy chunkSize bytes of data from sharedLocation into dataLocation, starting at sharedLocation
		memcpy(dataLocation, (void*)sharedLocation, chunkSize*sizeof(uint8_t));
		
		//update bytesLeft and dataLocation for the next iteration
		bytesTransferred = bytesTransferred + currentThread->getFileIOResult();
		dataLocation = ((uint8_t*)dataLocation + chunkSize);
	}

	if(currentThread->getFileIOResult() < 0){			//if call to open() failed
		MachineResumeSignals (&sigState);
		return VM_STATUS_FAILURE;											//return failure state
	}
	
	*length = bytesTransferred;
	MachineResumeSignals(&sigState);
	return VM_STATUS_SUCCESS;
}

TVMStatus VMFileWrite(int fileDescriptor, void* data, int *length){
	TMachineSignalState sigState;			
	MachineSuspendSignals(&sigState);	//suspend signals so we cannot be pre-empted while scheduling a new thread
	ThreadStore* tStore = ThreadStore::getInstance();
	MemoryPool* sharedMemory = tStore->findMemoryPoolByID((TVMMemoryPoolID)1);
	TCB* currentThread = tStore->getCurrentThread();
	uint8_t* sharedLocation;
	TVMMemorySize allocSize = *length;

	//Step 0 - validate data
	if((data == NULL) || (length == NULL)){
		MachineResumeSignals (&sigState);
		return VM_STATUS_ERROR_INVALID_PARAMETER;
	}
	
	//Step 1 - initialize shared memory location, and wait if memory is not available from share pool
	if(allocSize > 512){	//make sure thread never asks for > 512 bytes
		allocSize = 512;
	}
	if(allocSize > sharedMemory->getSize()){
		allocSize = sharedMemory->getSize();
	}

	sharedLocation = sharedMemory->allocateMemory(allocSize);		//allocate a space in the shared memory pool
	
	if(sharedLocation == NULL){
//		printf("VMFileWrite(): shared location was null\n");
		sharedLocation = tStore->waitCurrentThreadOnMemory(allocSize, 1);
	}

	//Step 2 - IO loop. If the data to transfer is > 512 bytes, loop. If it isn't, loop only runs once.
	int bytesLeft = *length;
	int chunkSize = bytesLeft;
	void *dataLocation = data;
	int bytesTransferred = 0;

	if(bytesLeft > allocSize)
		chunkSize = allocSize;

	for(bytesLeft; bytesLeft > 0; bytesLeft -= chunkSize){

		if(bytesLeft < chunkSize)
			chunkSize = bytesLeft;
		
		//printf("tid %d outputting\n", tStore->getCurrentThread()->getThreadID());

		//copy chunkSize bytes of data from *data into the shared memory location, starting at dataLocation
		memcpy((void*)sharedLocation, dataLocation, chunkSize*sizeof(uint8_t));
	
		//step 3 - call MachineFileWrite with the pointer to the shared memory location
		MachineFileWrite(fileDescriptor, sharedLocation, chunkSize, &machineFileIOCallback, (void*)currentThread);
		tStore->waitCurrentThreadOnIO();	//switch to a new thread while waiting on IO

		//update bytesLeft and dataLocation for the next iteration
		bytesTransferred = bytesTransferred + currentThread->getFileIOResult();
		dataLocation = ((uint8_t*)dataLocation + chunkSize);
	}

	//step 4 - Deallocate the shared memory location, do last error check, and return
	sharedMemory->deallocate(sharedLocation);
	
	if(bytesTransferred < 0){
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
		printf("thread to activate was null\n");
		MachineResumeSignals(&sigState);
		return VM_STATUS_ERROR_INVALID_ID;	//error status
	}
	
	if(threadToActivate->getState() != VM_THREAD_STATE_DEAD){	//thread exists but in wrong state
		printf("thread is in dead state\n");
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

TVMStatus VMStart(int tickms, TVMMemorySize heapSize, int machinetickms, TVMMemorySize sharedSize, int argc, char* argv[]){

	//tickms is the time in milliseconds for the alarm, this will be the quantum. 
	//machineticksms is the tick time of the machine, how long it will sleep in between actions.

	void* sharedMemoryBaseAddress = NULL;	
	const char* module = (const char*) argv[0];	//get the module from the command-line args
	sharedMemoryBaseAddress = MachineInitialize(machinetickms, (size_t)sharedSize);	

	ThreadStore* tStore = ThreadStore::getInstance();
	tStore->createSystemMemoryPool(heapSize);		//systemMemory is ID 0
	tStore->createSharedMemoryPool((uint8_t*)sharedMemoryBaseAddress, sharedSize);	//sharedMemory is id 1
	tStore->createMainThread();	//mainThread is ID 0
	tStore->createIdleThread();	//idleThread is ID 1

	//Make it so the threads' stacks are allocated from the memory pool!

	//request alarm every tickms USECONDS. alarm callback is schedule(high priority)
	MachineEnableSignals();	//enable signals	
	MachineRequestAlarm(1000*tickms, (TMachineAlarmCallback)&timerInterrupt, (void*)&defaultSchedulerPriority);	
	TVMMainEntry VMMain = VMLoadModule(module);							//load the module, save the VMMain function reference

	if(VMMain != NULL){													//if VMMain is a valid reference,
		VMMain(argc, argv);												//run the loaded module,
		//tStore->terminateCurrentThread();
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
