#include "Machine.h"
#include <list>
#include <vector>

class ThreadStore;
class TCB;
class Mutex;

typedef struct{
	Mutex *mutex;
	bool waiting;
}	MutexInfo;

extern "C"{

//Don't remove or comment out	==================================================================
TVMMainEntry VMLoadModule(const char* module); //They are implemented using Professor Nitta's provided source code in VirtualMachineUtils.c
void VMUnloadModule(void);
//===============================================================================================

void safeEntry(void *param);
void timerInterrupt(void *calldata);	//accepts a VM_THREAD_PRIORITY
void idle(void* parm1);
void machineFileIOCallback(void* calledParam, int result);
}

#ifndef MUTEX_H
#define MUTEX_H

class Mutex{
	
	public:
		
		Mutex();
		~Mutex();
		TCB* getOwner();
		bool isDeleted();
		TVMMutexID getID();
		void deleteFromVM();
		void lock(TCB* thread, TVMTick tick);
		void release();

	private:
		TVMMutexID id;
		bool deleted;
		TCB *owner;	
		std::list<TCB*>* priorityLists[3];
};

#endif


#ifndef TCB_H
#define TCB_H

class TCB{

	public:
		TCB(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid);
		~TCB();
		TVMThreadPriority getPriority();
		TVMThreadID getThreadID();
		void sleep(TVMTick tick);
		TVMThreadState getState();
		void setState(TVMThreadState state);
		void addMutex(Mutex *m, bool w);
		void setWaitingOnMutex(bool waiting);
		Mutex* getWaitingMutex();					//returns the tail of the mutexInfo list
		void removeMutex(Mutex *m);				//remove specified mutex from list
		bool isWaitingOnMutex();					//returns if TCB is waiting on last added mutex
		Mutex* getMutex();
		TVMTick getTicksToSleep();
		void decrementTicksToSleep();
		void setFileIOResult(int result);
		int getFileIOResult();	
		bool isWaitingOnIO();
		void setIsWaitingOnIO(bool waiting);
		bool isDeleted();
		void setIsDeleted(bool d);
		TVMThreadEntry getEntryFunction();
		SMachineContextRef contextRef;
		void releaseMutexes();

	private:
		TVMThreadEntry safeEntryFunction;		
		int fileIOResult;
		int** safeEntryParam;
		TVMThreadID threadID;
		TVMThreadPriority priority;
		TVMThreadState state;
		TVMThreadEntry entryFunction;
		bool deleted;
		void* entryParam;
		void* stackBaseAddress;
		TVMMemorySize stackSize;
		TVMTick ticksToSleep;						//how many ticks to sleep the thread before awaking it
		bool waitingOnIO;
		std::list<MutexInfo*> *mutexes;
};

#endif

#ifndef MEMORYPOOL_H
#define MEMORYPOOL_H

class MemoryPool{

	public:
		MemoryPool();
		~MemoryPool();
		TVMMemoryPoolID getID();
		TVMMemorySize getSize();

	private:
		uint8_t *baseAddress;
		TVMMemorySize poolSize;
		TVMMemoryPoolID id;

};


#endif


#ifndef THREADSTORE_H
#define THREADSTORE_H

class ThreadStore{
	
	public:
		static ThreadStore *getInstance();
		TVMMemoryPoolID getNumMemoryPools();
		~ThreadStore();
		TVMMutexID getNumMutexes();
		Mutex* findMutexByID(TVMMutexID mutexID);
		MemoryPool* findMemoryPoolByID(TVMMemoryPoolID id);
		void insert(Mutex *mutex);
		void insert(TCB *tcb);
		void scheduleThreadEarly(TCB* thread);
		int getNumThreads();
		void createIdleThread();
		void createMainThread();
		TCB* getCurrentThread();
		void sleepCurrentThread(TVMTick tick);
		void schedule(TVMThreadPriority priority);
		void timerEvent();
		TCB* findThreadByID(TVMThreadID ID);
		void activateDeadThread(TCB* deadThread);
		int voidParam;
		bool isThreadPrioInRange(TCB*);	//debug, remove
		void switchToNewThread(TCB* thread);
		void waitCurrentThreadOnIO();
		void removeFromWaitlistEarly(TCB *thread);
		void terminate(TCB* thread);
		void terminateCurrentThread();
		void deleteDeadThread(TCB* thread);
		void setSharedMemoryParams(void *sharedMemoryBaseAddress, TVMMemorySize sharedsize);

	protected:

		ThreadStore();
		void *sharedMemoryBaseAddress;
		TVMMemorySize heapSize;
		static ThreadStore *DUniqueInstance;
		TVMMemoryPoolID numMemoryPools;
		TVMThreadID numThreads;
		TVMMutexID numMutexes;
		TCB* idleThread;
		TCB* currentThread;
		TVMThreadID idleThreadID;
		std::list<TCB*>* readyLists[4];
		std::list<TCB*> *waitList;
		std::list<TCB*> *deadList;
		std::vector<TCB*> *allThreads;
		std::vector<Mutex*> *mutexVector;
		std::vector<MemoryPool*> *memoryPoolVector;
};

#endif
