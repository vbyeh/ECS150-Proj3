#include "VirtualMachine.h"
#include "Machine.h"
#include "UtilityFunctions.h"
#include <stdint.h>
#include <stdio.h>

//=== Mutex =======================================================

Mutex::Mutex(){
	ThreadStore *tStore = ThreadStore::getInstance();
	id									= 	tStore->getNumMutexes() + 1;	//never decreases, here or in ThreadStore
	owner			 					= NULL;	//also used as the lock state
	priorityLists[0x00] = new std::list<TCB*>();
	priorityLists[0x01] = new std::list<TCB*>();
	priorityLists[0x02] = new std::list<TCB*>();
}

Mutex::~Mutex(){

	for(int i = 0x0; i < 0x03; i++){
		while(priorityLists[i]->empty() == false){	//while the list isn't empty
			priorityLists[i]->pop_front();						//take stuff out of it
		}
		delete priorityLists[i];										//once it's empty, delete it
	}
}

void Mutex::release(){

	ThreadStore *tStore = ThreadStore::getInstance();
	TVMThreadPriority oldPriority = owner->getPriority();
	owner->removeMutex(this);	//the old owner is done, so isn't waiting on this mutex anymore

	if((priorityLists[0x00]->empty() == true) && (priorityLists[0x01]->empty() == true) && (priorityLists[0x02]->empty() == true)){
		//all prios empty, mutex has no owner and schedules no new threads
		owner = NULL;
	}
	else{																								//at least one prio is waiting
		for(TVMThreadPriority i = 0x02; i >= 0x00; i--){	//check prios starting high
			if(priorityLists[i]->empty() == false){					//if another thread waiting on mutex
				owner = priorityLists[i]->front();						//set new mutex owner to be that thread
				priorityLists[i]->pop_front();								//pop	front, as thread no longer waiting
				break;																				//stop looking for new thread
			}
		}
		TVMThreadState savedState = owner->getState();
		owner->sleep(0);	//set ticks to sleep = 0
		owner->setState(savedState);
		//release of mutex causes higher-prio thread to be scheduled
		if(owner->getPriority() > oldPriority){	//if new thread, eg thread that was waiting, more important than old
			tStore->scheduleThreadEarly(owner);		//remove from waitlist early and run scheduler
		//	tStore->removeFromWaitlistEarly(owner);
		//	schedule(VM_THREAD_PRIORITY_HIGH);
		}
		else
			tStore->removeFromWaitlistEarly(owner);
	}
}

void Mutex::lock(TCB *thread, TVMTick timeout){	//called from VMMutexAcquire
	ThreadStore *tStore = ThreadStore::getInstance();
	TCB* currentThread = tStore->getCurrentThread();

	if(owner == NULL){	//if not owned
		owner = tStore->getCurrentThread();	//give to asking thread
		currentThread->addMutex(this, false);			//add mutex, not waiting on it
		return;
	}
	
	if((timeout == VM_TIMEOUT_IMMEDIATE) && (owner != NULL)){	
		return;	//return immediately if mutex is locked, don't add to mutexes list
	}
	else{
		thread->setWaitingOnMutex(true);	//set thread waiting on mutex
		if(timeout == VM_TIMEOUT_INFINITE){
			tStore->getCurrentThread()->addMutex(this, true);		//add mutex, waiting
			priorityLists[currentThread->getPriority() - 1]->push_back(currentThread);	//add thread to mutex wait list
			tStore->sleepCurrentThread(-1);	//insert thread into waitlist with ticksToSleep = timeout
		}
		else{
			tStore->getCurrentThread()->addMutex(this, true);	//add mutex, waiting
			priorityLists[currentThread->getPriority() - 1]->push_back(currentThread);	//add thread to mutex wait list
			tStore->sleepCurrentThread(timeout);
		}
	}
}

void Mutex::deleteFromVM(){
	for(int i = 0x0; i < 0x03; i++){	//a thread cannot wait on a deleted mutex
		while(priorityLists[i]->empty() == false){	//while the list isn't empty
			priorityLists[i]->front()->removeMutex(this);					//indicate threads are no longer waiting on this mutex
			priorityLists[i]->pop_front();						//take stuff out of it
		}
	}
	owner 	= NULL;	//a deleted mutex cannot be held
	deleted = true;
}

bool Mutex::isDeleted(){
	return deleted;
}

TCB* Mutex::getOwner(){	//a free mutex has a null owner
	return owner;
}

TVMMutexID Mutex::getID(){
	return id;
}

//=================================================================


//=== TCB =======================================================

extern "C" {
	TCB::~TCB(){
		delete (uint8_t*)(stackBaseAddress);	//free memory called by new
		safeEntryParam[0] = NULL;
		safeEntryParam[1] = NULL;
		delete safeEntryParam;
		delete contextRef;
	}
	
	TCB::TCB(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid){
		//Initialize all variables in thread control block
		//These initializations are common to all threads:

		//assign IDs in ascending order, return the ID to the calling function by reference
		threadID = ThreadStore::getInstance()->getNumThreads() + 1;
		*tid = threadID;

		TVMThreadEntry safeEntryFunction 	= &safeEntry;				//this is the skeleton function
		safeEntryParam 										= new int*[2];
		safeEntryParam[0]									= (int *)entry;			//skeleton function's params - [0] is thread entry func, [1] is param for thread entry func
		safeEntryParam[1]									= (int *)param;
	
		mutexes 					= new std::list<MutexInfo*>();		//the mutex the thread is waiting on. Threads dont start with a mutex.
		deleted		 				= false;
		priority 				 	= prio;
		entryFunction 		= entry;
		entryParam 				= param;			//cast param to int*, assign
		stackSize 			 	= memsize;
		fileIOResult 			= 0;
		ticksToSleep		 	= 0;
		contextRef 				= new SMachineContext[1];
		waitingOnIO 			= false;
		
		if(entry == (TVMThreadEntry)&VMStart){				//indicates creation of vmStart thread, which already has a context 
			//vmStart is running
			state 					 = VM_THREAD_STATE_RUNNING;			//vmStart is obviously running
			stackBaseAddress = new uint8_t[1];							//may need to change this later
		}
		else if(entry == (TVMThreadEntry)&idle){					//initializing idle thread
			state = VM_THREAD_STATE_READY;
			stackBaseAddress 	= new uint8_t[memsize];				//allocate stack
			//assign context to new machine context, e.g. context = Machine->newContext(params...);
			MachineContextCreate(contextRef, &idle, NULL, stackBaseAddress, stackSize);
		}
		else{	
			state 					 	= VM_THREAD_STATE_DEAD;				//All other threads begin in dead state
			stackBaseAddress 	= new uint8_t[memsize];				//allocate stack
			//assign context to new machine context, e.g. context = Machine->newContext(params...);
			MachineContextCreate(contextRef, safeEntryFunction, safeEntryParam, stackBaseAddress, stackSize);
		}
	}
}

void TCB::setIsDeleted(bool d){
	deleted = d;
}

bool TCB::isDeleted(){
	return deleted;
}

void TCB::setFileIOResult(int result){
	fileIOResult = result;
}

int TCB::getFileIOResult(){
	return fileIOResult;
}

void TCB::addMutex(Mutex *m, bool w){
	MutexInfo *mInfo = new MutexInfo();
	mInfo->mutex = m;
	mInfo->waiting = w;
	mutexes->push_back(mInfo);
}

void TCB::removeMutex(Mutex *m){

	for(std::list<MutexInfo*>::iterator i = mutexes->begin(); i != mutexes->end(); i++){	//iterate over entire wait list
		if((*i)->mutex == m){
			mutexes->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;
		}
	}
}

Mutex* TCB::getWaitingMutex(){
	if(mutexes->empty() == false){
		return mutexes->back()->mutex;
	}
	else
		return NULL;
}

bool TCB::isWaitingOnMutex(){
	if(mutexes->empty() == false){
		return mutexes->back()->waiting;
	}
	else{
		return false;
	}
}

void TCB::setWaitingOnMutex(bool w){
	if(mutexes->empty() == false){
		mutexes->back()->waiting = w;
	}
}

void TCB::setIsWaitingOnIO(bool waiting){
	waitingOnIO = waiting;
}

bool TCB::isWaitingOnIO(){
	return waitingOnIO;
}

void TCB::releaseMutexes(){
	while(mutexes->empty() == false){
		MutexInfo *m = mutexes->front();
		mutexes->pop_front();
		m->mutex->release();
		delete m;
	}
}

TVMThreadID TCB::getThreadID(){
	return threadID;
}

TVMThreadPriority TCB::getPriority(){
	return priority;
}

TVMThreadState TCB::getState(){
	return state;
}

TVMThreadEntry TCB::getEntryFunction(){
	return entryFunction;
}

void TCB::setState(TVMThreadState s){
	state = s;
}

void TCB::sleep(TVMTick tick){
	state = VM_THREAD_STATE_WAITING;	//puts the thread into the waiting state for tick ticks
	ticksToSleep = tick;	//sets how long the thread should sleep
}

TVMTick TCB::getTicksToSleep(){
	return ticksToSleep;
}

void TCB::decrementTicksToSleep(){
	ticksToSleep = ticksToSleep - 1;
}

//=================================================================


//=== MemoryPool ==================================================

MemoryPool::MemoryPool(){

		id = ThreadStore::getInstance()->getNumMemoryPools() + 1;
}

MemoryPool::~MemoryPool(){

}




//=================================================================


//=== ThreadStore ==================================================

ThreadStore *ThreadStore::DUniqueInstance = NULL;

void ThreadStore::sleepCurrentThread(TVMTick tick){

	currentThread->sleep(tick);	//set current thread's ticksToSleep to tick, set state to WAITING

	if(tick == 0)															//if sleep was called with timeout immediate
		schedule(currentThread->getPriority());	//attempt to schedule new thread of current priority
	else
		schedule(VM_THREAD_PRIORITY_HIGH);			//attempt to schedule high priority thread
}

Mutex* ThreadStore::findMutexByID(TVMMutexID mutexID){
	if((mutexID >= mutexVector->size()) || (mutexID < 0)){
		return NULL;							//mutexID is out of bounds
	}
	return (*mutexVector)[mutexID];	//otherwise return the mutex
}

TVMMutexID ThreadStore::getNumMutexes(){
	return numMutexes;
}

void ThreadStore::removeFromWaitlistEarly(TCB* thread){
	
	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i) == thread) && ((*i)->isWaitingOnIO() == false)){	//if thread found and thread not waiting on IO
			thread->sleep(0);																				//set ticks to sleep zero
			thread->setState(VM_THREAD_STATE_READY);								//set state ready
			readyLists[thread->getPriority()]->push_back(thread);		//put thread in ready list
			waitList->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;																									//stop looking
		}
	}
}

void ThreadStore::scheduleThreadEarly(TCB* thread){
	//finds given thread in waitlist, takes it out of waitlist, runs scheduler
	TCB* foundThread = NULL;
	
	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i) == thread) && ((*i)->isWaitingOnIO() == false)){	//if thread found and thread not waiting on IO
			foundThread = (*i);
			thread->sleep(0);																				//set ticks to sleep zero
			thread->setState(VM_THREAD_STATE_READY);								//set state ready
			readyLists[thread->getPriority()]->push_back(thread);		//put thread in ready list
			waitList->erase(i);																			//remove thread from wait list
			i--;																										//decrement iterator
			break;																									//stop looking
		}
	}
	if(foundThread == thread){	//if a thread was found
		schedule(VM_THREAD_PRIORITY_HIGH);	//run scheduler, is outside FOR because schedule does not return
	}
}

void ThreadStore::waitCurrentThreadOnIO(){

	currentThread->setIsWaitingOnIO(true);
	currentThread->setState(VM_THREAD_STATE_WAITING);
	schedule(VM_THREAD_PRIORITY_HIGH);
	return;
}

void ThreadStore::deleteDeadThread(TCB* thread){

	for(std::list<TCB*>::iterator TCBItr = deadList->begin(); TCBItr != deadList->end(); TCBItr++){		//check deadList
		if((*TCBItr)->getThreadID() == thread->getThreadID()){	//if found id matches requested id
			//deadList->erase(TCBItr);
			thread->setIsDeleted(true);
			//delete thread;
			break;
		}
	}
	schedule(VM_THREAD_PRIORITY_HIGH);
}

TCB* ThreadStore::findThreadByID(TVMThreadID id){
	//searches every container for a thread matching id

	if(currentThread->getThreadID() == id){
		return currentThread;
	}
	for(TVMThreadPriority priority = 0x01; priority < 0x04; priority++){	//check all ready lists except idle list
		for(std::list<TCB*>::iterator TCBItr = readyLists[priority]->begin(); TCBItr != readyLists[priority]->end(); TCBItr++){
			//check every item in readyList[priority]
			if((*TCBItr)->getThreadID() == id){	//if found id matches requested id
				return *TCBItr;	//return TCB*
			}
		}
	}
	
	for(std::list<TCB*>::iterator TCBItr = waitList->begin(); TCBItr != waitList->end(); TCBItr++){ //check wait list
		if((*TCBItr)->getThreadID() == id){	//if found id matches requested id
			return *TCBItr;	//return TCB*
		}
	}
	
	for(std::list<TCB*>::iterator TCBItr = deadList->begin(); TCBItr != deadList->end(); TCBItr++){			//check deadList
		if(((*TCBItr)->getThreadID() == id) && ((*TCBItr)->isDeleted() == false)){	//if found id matches requested id and thread not deleted
			return *TCBItr;	//return TCB*
		}
	}
	return NULL; //if code gets here, ID was not found
}

void ThreadStore::activateDeadThread(TCB* deadThread){

	TCB* foundThread = NULL;
	//create a context for the thread

	for(std::list<TCB*>::iterator TCBItr = deadList->begin(); TCBItr != deadList->end(); TCBItr++){
	
		if(*TCBItr == deadThread){	//if found TCB matches given TCB
			foundThread = *TCBItr;
			break;
		}
	}
	if(foundThread == deadThread){
		TVMThreadPriority priority = foundThread->getPriority();	//get priority of found thread
		foundThread->setState(VM_THREAD_STATE_READY);							//set thread state to READY
		readyLists[priority]->push_back(foundThread);								//add to back of ready list - make this front?
		if(foundThread->getPriority() > currentThread->getPriority()){
			schedule(VM_THREAD_PRIORITY_HIGH);
		}
	}
}

void ThreadStore::terminate(TCB* thread){

	if(thread->getEntryFunction() != &idle){
		thread->releaseMutexes();
		thread->setState(VM_THREAD_STATE_DEAD);	//mark thread as dead
		deadList->push_back(thread);							//push to dead list
		//delete the thread's context
		schedule(VM_THREAD_PRIORITY_HIGH);
	}
	return;
}

void ThreadStore::terminateCurrentThread(){
	terminate(currentThread);
}


void ThreadStore::switchToNewThread(TCB* nextThread){

	nextThread->setState(VM_THREAD_STATE_RUNNING);		//set next thread to running
	//if currentThread dead or idle, DON'T push to wait list!	

	if(currentThread->getEntryFunction() != &idle){	//if current thread is idle, skip

		if(currentThread->getState() == VM_THREAD_STATE_WAITING){
			waitList->push_back(currentThread);	//add current thread - one to be swapped out - to wait list
		}
		else if(currentThread->getState() == VM_THREAD_STATE_DEAD){	//activating a dead thread
		}
		else if(currentThread->getState() == VM_THREAD_STATE_READY){	//activating a dead thread
			readyLists[currentThread->getPriority()]->push_back(currentThread);	//go back in ready queue
		}
		else if(currentThread->getState() == VM_THREAD_STATE_RUNNING){
			currentThread->setState(VM_THREAD_STATE_READY);	//put state to ready
			readyLists[currentThread->getPriority()]->push_back(currentThread);	//go back in ready queue
		}
	}

	SMachineContextRef oldContext = currentThread->contextRef;	//old context is current thread's
	SMachineContextRef newContext = nextThread->contextRef;			//new context is next thread's
	currentThread = nextThread;																	//set current active thread to be next thread
	MachineContextSwitch(oldContext, newContext);
	return;
}

void ThreadStore::schedule(TVMThreadPriority priority){ //priority = thread priority to schedule first. default is HIGH.

	//Checks if there are any threads ready to run in any ready lists
	//If no threads, runs idle thread until timerEvent is called at next timeslice

	bool areAllListsEmpty = (readyLists[0x01]->empty() && readyLists[0x02]->empty() && readyLists[0x03]->empty());

	if((areAllListsEmpty == true) && (currentThread->getState() == VM_THREAD_STATE_RUNNING)){
		return;
	}

	if(readyLists[priority]->empty() == false){	//attempt to swap in thread of given priority
		TCB* thread = readyLists[priority]->front();
		if(priority != 0x00){												//if list is not special idle thread queue
			readyLists[priority]->pop_front();				//pop old pointer from the list
		}
		switchToNewThread(thread);	//switch to new thread of priority priority
	}
	else{
		TCB* newThread;																								//no threads of given priority exist
		for(TVMThreadPriority i = 0x03; i >= 0x00; i--){	//Check highest prio and work down
			if(i == priority){										//skip priority level that was already checked
				continue;
			}
			if(readyLists[i]->empty() == false){ 		//Queue is not empty
				newThread = readyLists[i]->front();
				if(i != 0x0){												//if list is not special idle thread queue
					readyLists[i]->pop_front();				//pop old pointer from the list
				}
				break;
			}
		}
		switchToNewThread(newThread);
	}
}

void ThreadStore::timerEvent(){
	//checks wait list for any threads that have finished sleeping
	//moves threads that are done sleeping from wait list into appropriate ready list
	//ThreadStore::schedule() is then called, which swaps out the current thread for a thread from a ready list

	for(std::list<TCB*>::iterator i = waitList->begin(); i != waitList->end(); i++){	//iterate over entire wait list
		if(((*i)->isWaitingOnMutex() == true) && ((*i)->getWaitingMutex() != NULL)){	//if waiting on AT LEAST ONE mutex
			if((*i) == (*i)->getWaitingMutex()->getOwner()){	//if mutex was acquired, ie if mutex owner is same as waiting thread
				//waitingMutex is necessarily the mutex thread i most recently tried to acquire
				(*i)->setWaitingOnMutex(false);		//no longer waiting
				(*i)->sleep(0);		//set ticks to sleep = 0
			}
		}
		if((*i)->isWaitingOnIO() == true){	//if the thread is waiting on IO, ignore it, even if it has mutex
			continue;
		}
		if(((*i)->getTicksToSleep() != (unsigned)(-1)) && ((*i)->getTicksToSleep() > 0)){	//-1 means it will sleep infinitely
				//if thread still has ticks to sleep and is not sleeping infinitely, decrement ticks by 1
			(*i)->decrementTicksToSleep();
		}
		else if((*i)->getTicksToSleep() == (unsigned)(-1)){	//-1 means it will sleep infinitely
			continue;
		} 
		else if((*i)->getTicksToSleep() == 0){	//if thread has no ticks to sleep, put in ready list, even if no mutex
			(*i)->setState(VM_THREAD_STATE_READY);
			readyLists[(*i)->getPriority()]->push_back(*i);			//insert thread into ready list
			waitList->erase(i);															//remove thread from wait list
			i--;
		}
	}

	if((readyLists[0x01]->empty() && readyLists[0x02]->empty() && readyLists[0x03]->empty())){
		if(waitList->empty() && (currentThread->getEntryFunction() == &idle)){
			//if all ready lists are empty, the wait list is empty, and the idle thread is running, call MachineTerminate() here
			VMUnloadModule();
			MachineTerminate();
			return;
		}
	}
	schedule(VM_THREAD_PRIORITY_HIGH);
}

ThreadStore::ThreadStore(){
	
	for(TVMThreadPriority i = 0x0; i < 0x04; i++){
		//priority lists: 0x0 is idle, 0x1 is low, 0x2 is mid, 0x3 is high
		readyLists[i] = new std::list<TCB*>();	
	}

	allThreads = new std::vector<TCB*>();
	waitList = new std::list<TCB*>();			//the list of threads in waiting state
	deadList = new std::list<TCB*>();			//the list of threads in dead state
	mutexVector = new std::vector<Mutex*>();	//vector of all mutexes
	memoryPoolVector = new std::vector<MemoryPool*>();	//vector of all mutexes
	numThreads = -1;
	numMutexes = -1;
	numMemoryPools = -1;
	voidParam = 0;
	idleThreadID = 0;		//initialize the idle thread's id to zero, it will be overwritten
}

ThreadStore *ThreadStore::getInstance(){
    if(NULL == DUniqueInstance){
        DUniqueInstance = new ThreadStore();
    }
    return DUniqueInstance;
}

void ThreadStore::setSharedMemoryParams(void *smba, TVMMemorySize hs){
	sharedMemoryBaseAddress = smba;
	heapSize = hs;
}

TCB* ThreadStore::getCurrentThread(){
	return currentThread;
}

bool ThreadStore::isThreadPrioInRange(TCB* tcb){

	//if the thread's priority is less than 1 or greater than 3
	if((tcb->getPriority() < (TVMThreadPriority)0x01) || (tcb->getPriority() > (TVMThreadPriority)0x03)){
		return false;	//false = we are out of range = bad
	}
	else
		return true;	//we are not out of range so we are in range
}

void ThreadStore::insert(TCB* tcb){	
//insert a newly created thread into the correct container
//currently, only the dead case should be called

	TVMThreadPriority priority = tcb->getPriority();
	TVMThreadState state = tcb->getState();
	allThreads->push_back(tcb);
	
	switch(state){
		
		case VM_THREAD_STATE_DEAD:
			deadList->push_back(tcb);
			break;
		case VM_THREAD_STATE_WAITING:
			waitList->push_back(tcb);
		case VM_THREAD_STATE_READY:
			readyLists[priority]->push_back(tcb);			//otherwise go ahead and add it
			//lists[0x01] is low, queues[0x02] is mid, queues[0x03] is high
			break;
		default:
			break;
	}
	numThreads++;	//increment number of threads
}

void ThreadStore::insert(Mutex *mutex){
//adds a mutex to the vector of mutexes
	mutexVector->push_back(mutex);
	numMutexes++;
}

void ThreadStore::createMainThread(){

	//initialize parameters for creating vmStart thread
	TVMThreadEntry vmStartEntry = (TVMThreadEntry)&VMStart;
	int vmStartParam = 0;
	TVMMemorySize vmStartMemsize = 0;	//arbitrary
	TVMThreadPriority vmStartPrio = VM_THREAD_PRIORITY_NORMAL;
	TVMThreadID vmStartTID = 0xFFFFFFFF;	//will be overwritten with 1
	TCB* vmStartThread = new TCB(vmStartEntry, (void*)&vmStartParam, vmStartMemsize, vmStartPrio, (TVMThreadIDRef)&vmStartTID);
	insert(vmStartThread);
	currentThread = vmStartThread;
}

void ThreadStore::createIdleThread(){

	TCB* idleThread;

	if(readyLists[0x0]->empty() == true){	//if no idle thread exists
		TVMMemorySize m = 0x100000;
		TVMThreadPriority p = 0x00;
		//idle thread has its own special list
		idleThread = new TCB((TVMThreadEntry)&idle, (void *)(voidParam), m, p, &idleThreadID);
		readyLists[0x0]->push_back(idleThread);
		allThreads->push_back(idleThread);
		numThreads++;
	}
}

TVMMemoryPoolID ThreadStore::getNumMemoryPools(){
	return numMemoryPools;
}

int ThreadStore::getNumThreads(){
	return numThreads;
}

ThreadStore::~ThreadStore(){

	while(allThreads->empty() == false){
		TCB* thread = allThreads->back();
		if(thread != NULL)
			delete thread;
		allThreads->pop_back();
	}
	delete allThreads;

	while(mutexVector->empty() == false){
		Mutex *mutex = mutexVector->back();
		mutexVector->pop_back();
		delete mutex;
	}
	delete mutexVector;	

	for(TVMThreadPriority i = 0x00; i < 0x04; i++){	//iterate over all ready lists
		while(readyLists[i]->empty() == false){				//while the list is not empty
			readyLists[i]->pop_front();									//pop off head of list
		}
		delete readyLists[i];													//delete empty container
	}

	while(waitList->empty() == false){
		waitList->pop_front();						//erase head of list
	}
	delete waitList;

	while(deadList->empty() == false){
		deadList->pop_front();						//erase head of list
	}
	delete deadList;
}

//=================================================================



//=== Utilities ==================================================

void safeEntry(void *functionAndParams){
	//this is the skeleton function
	MachineEnableSignals();

	ThreadStore *tStore = ThreadStore::getInstance();
//	printf("safeEntry(): entering thread TID %d\n", tStore->getCurrentThread()->getThreadID());
	TVMThreadEntry entry 	= (TVMThreadEntry)(((int**)functionAndParams)[0]);
	void *params 					= (void *)(((int**)functionAndParams)[1]);
	entry(params);				//this starts the thread
//	printf("safeEntry(): returned, terminating TID %d\n", tStore->getCurrentThread()->getThreadID());
	tStore->terminateCurrentThread();
	//this is called if user does not terminate on their own
}

void idle(void* parm1){	//infinite loop for the idle thread
	MachineEnableSignals();
	while(1);
}

void timerInterrupt(void *calldata){
	TMachineSignalState sigState;
	MachineSuspendSignals(&sigState);		
	ThreadStore::getInstance()->timerEvent();	//scheduling logic is in the threadstore because that's where all the TCBs are
	MachineResumeSignals(&sigState);		//resume signals once scheduling ops are done
	return;
}

void machineFileIOCallback(void* calledParam, int result){

	((TCB*)calledParam)->setFileIOResult(result);	//return file descriptor to original location by reference. will be -1 if an error occurs
	((TCB*)calledParam)->setIsWaitingOnIO(false);
	return;
}
//==================================================================
