#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include "LED_Test.h"
#include "os.h"
#include "queue.h"

//Comment out the following line to remove debugging code from compiled version.
#define DEBUG

extern void a_main();

/*===========
  * RTOS Internal
  *===========
  */

/**
  * This internal kernel function is the context switching mechanism.
  * It is done in a "funny" way in that it consists two halves: the top half
  * is called "Exit_Kernel()", and the bottom half is called "Enter_Kernel()".
  * When kernel calls this function, it starts the top half (i.e., exit). Right in
  * the middle, "Cp" is activated; as a result, Cp is running and the kernel is
  * suspended in the middle of this function. When Cp makes a system call,
  * it enters the kernel via the Enter_Kernel() software interrupt into
  * the middle of this function, where the kernel was suspended.
  * After executing the bottom half, the context of Cp is saved and the context
  * of the kernel is restore. Hence, when this function returns, kernel is active
  * again, but Cp is not running any more. 
  * (See file "switch.S" for details.)
  */
extern void CSwitch();
extern void Exit_Kernel();    /* this is the same as CSwitch() */

/* Prototype */
void Task_Terminate(void);
static void Dispatch();
static void Kernel_Unlock_Mutex();

/** 
  * This external function could be implemented in two ways:
  *  1) as an external function call, which is called by Kernel API call stubs;
  *  2) as an inline macro which maps the call into a "software interrupt";
  *       as for the AVR processor, we could use the external interrupt feature,
  *       i.e., INT0 pin.
  *  Note: Interrupts are assumed to be disabled upon calling Enter_Kernel().
  *     This is the case if it is implemented by software interrupt. However,
  *     as an external function call, it must be done explicitly. When Enter_Kernel()
  *     returns, then interrupts will be re-enabled by Enter_Kernel().
  */ 
extern void Enter_Kernel();

/**
  * This table contains ALL process descriptors. It doesn't matter what
  * state a task is in.
  */
static PD Process[MAXTHREAD];

static MTX Mutex[MAXMUTEX];

static EVT Event[MAXEVENT];

/**
  * The process descriptor of the currently RUNNING task.
  */
volatile static PD* Cp; 

/** 
  * Since this is a "full-served" model, the kernel is executing using its own
  * stack. We can allocate a new workspace for this kernel stack, or we can
  * use the stack of the "main()" function, i.e., the initial C runtime stack.
  * (Note: This and the following stack pointers are used primarily by the
  *   context switching code, i.e., CSwitch(), which is written in assembly
  *   language.)
  */         
volatile unsigned char *KernelSp;

/**
  * This is a "shadow" copy of the stack pointer of "Cp", the currently
  * running task. During context switching, we need to save and restore
  * it into the appropriate process descriptor.
  */
volatile unsigned char *CurrentSp;

/** 1 if kernel has been started; 0 otherwise. */
volatile static unsigned int KernelActive;  

/** number of tasks created so far */
volatile static unsigned int Tasks;  
volatile static unsigned int pCount;

// Number of mutexes created so far
volatile static unsigned int Mutexes;

volatile static unsigned int Events;

// Global tick overflow count
volatile unsigned int tickOverflowCount = 0;

volatile PD *ReadyQueue[MAXTHREAD];
volatile int RQCount = 0;

volatile PD *SleepQueue[MAXTHREAD];
volatile int SQCount = 0;

volatile PD *WaitingQueue[MAXTHREAD];
volatile int WQCount = 0;

/**
 * When creating a new task, it is important to initialize its stack just like
 * it has called "Enter_Kernel()"; so that when we switch to it later, we
 * can just restore its execution context on its stack.
 * (See file "cswitch.S" for details.)
 */
PID Kernel_Create_Task_At( volatile PD *p, voidfuncptr f, PRIORITY py, int arg ) {   
	unsigned char *sp;

#ifdef DEBUG
	int counter = 0;
#endif

	//Changed -2 to -1 to fix off by one error.
	sp = (unsigned char *) &(p->workSpace[WORKSPACE-1]);



	/*----BEGIN of NEW CODE----*/
	//Initialize the workspace (i.e., stack) and PD here!

	//Clear the contents of the workspace
	memset(&(p->workSpace),0,WORKSPACE);

	//Notice that we are placing the address (16-bit) of the functions
	//onto the stack in reverse byte order (least significant first, followed
	//by most significant).  This is because the "return" assembly instructions 
	//(rtn and rti) pop addresses off in BIG ENDIAN (most sig. first, least sig. 
	//second), even though the AT90 is LITTLE ENDIAN machine.

	//Store terminate at the bottom of stack to protect against stack underrun.
	*(unsigned char *)sp-- = ((unsigned int)Task_Terminate) & 0xff;
	*(unsigned char *)sp-- = (((unsigned int)Task_Terminate) >> 8) & 0xff;

	//Place return address of function at bottom of stack
	*(unsigned char *)sp-- = ((unsigned int)f) & 0xff;
	*(unsigned char *)sp-- = (((unsigned int)f) >> 8) & 0xff;
	*(unsigned char *)sp-- = 0x00; // Fix 17 bit address problem for PC

#ifdef DEBUG
   //Fill stack with initial values for development debugging
   //Registers 0 -> 31 and the status register
	for (counter = 0; counter < 34; counter++) {
		*(unsigned char *)sp-- = counter;
	}
#else
	//Place stack pointer at top of stack
	sp = sp - 34;
#endif
	  
	p->sp = sp;     /* stack pointer into the "workSpace" */
	p->code = f;        /* function to be executed as a task */
	p->request = NONE;
	p->p = pCount;
	p->py = py;
	p->inheritedPy = py;
	p->arg = arg;
	p->suspended = 0;
	p->eWait = 99;

	Tasks++;
	pCount++;

	/*----END of NEW CODE----*/

	p->state = READY;

	// Add to ready queue
	enqueueRQ(&p, &ReadyQueue, &RQCount);

	return p->p;
}

/**
  *  Create a new task
  */
static PID Kernel_Create_Task( voidfuncptr f, PRIORITY py, int arg ) {
	int x;

	if (Tasks == MAXTHREAD) return;  /* Too many task! */

	/* find a DEAD PD that we can use  */
	for (x = 0; x < MAXTHREAD; x++) {
		if (Process[x].state == DEAD) break;
	}

	unsigned int p = Kernel_Create_Task_At( &(Process[x]), f, py, arg );

	return p;
}

static void Kernel_Suspend_Task() {
	int i;

	if(Cp->p == Cp->pidAction) {
		Cp->suspended = 1;
	}
	else {
		for(i = 0; i < MAXTHREAD; i++) {
			if (Process[i].p == Cp->pidAction) break;
		}

		Process[i].suspended = 1;
	}
}

static unsigned int Kernel_Resume_Task() {
	int i;

	for(i = 0; i < MAXTHREAD; i++) {
		if (Process[i].p == Cp->pidAction) break;
	}

	if(Process[i].suspended == 1) {
		Process[i].suspended = 0;
		if(Process[i].inheritedPy < Cp->inheritedPy) {
			return 1;
		}
	}

	return 0;
}

static void Kernel_Terminate_Task() {
	Cp->inheritedPy = 0;
	Cp->py = 0;
	Cp->state = TERMINATED;

	int i;

	for(i = 0; i < MAXMUTEX; i++) {
		if (Mutex[i].owner == Cp->p) {
			Cp->m = Mutex[i].m;
			Kernel_Unlock_Mutex();
		}
	}

	Cp->state = DEAD;
	Cp->eWait = 99;
	Cp->inheritedPy = MINPRIORITY;
	Cp->py = MINPRIORITY;
	Cp->p = 0;
	Tasks--;
}

MUTEX Kernel_Init_Mutex_At(volatile MTX *m) {
	m->m = Mutexes;
	m->state = FREE;
	Mutexes++;

	return m->m;
}

static MUTEX Kernel_Init_Mutex() {
	int x;

	if (Mutexes == MAXMUTEX) return; // Too many mutexes!

	// find a Disabled mutex that we can use
	for (x = 0; x < MAXMUTEX; x++) {
		if (Mutex[x].state == DISABLED) break;
	}

	unsigned int m = Kernel_Init_Mutex_At( &(Mutex[x]) );

	return m;
}

static unsigned int Kernel_Lock_Mutex() {
	int i,j;
	MUTEX m = Cp->m;

	for(i = 0; i < MAXMUTEX; i++) {
		if (Mutex[i].m == m) break;
	}

	if(i>=MAXMUTEX){
		return 1;
	}

	if(Mutex[i].state == FREE) {
		Mutex[i].state = LOCKED;
		Mutex[i].owner = Cp->p;
		Mutex[i].lockCount++;
	}
	else if (Mutex[i].owner == Cp->p) {
		Mutex[i].lockCount++;
	}
	else {
		for(j = 0; j < MAXTHREAD; j++) {
			if ((Process[j].p == Mutex[i].owner) && (Process[j].p != 0)) break;
		}

		if (Process[j].inheritedPy > Cp->inheritedPy) {
			Process[j].inheritedPy = Cp->inheritedPy;
		}

		Cp->state = BLOCKED_ON_MUTEX;
		enqueueWQ(&Cp, &WaitingQueue, &WQCount);

		return 0;
	}

	return 1;
}

static void Kernel_Unlock_Mutex() {
	int i;
	MUTEX m = Cp->m;

	for(i = 0; i < MAXMUTEX; i++) {
		if (Mutex[i].m == m) break;
	}

	if(i >= MAXMUTEX){
		return;
	}

	if(Mutex[i].owner != Cp->p){
		return;
	} 
	else if (Cp->state == TERMINATED) {
		volatile PD* p = dequeueWQ(&WaitingQueue, &WQCount, m);
		if (p == NULL) {
			Mutex[i].lockCount = 0;
			Mutex[i].state = FREE;
			Mutex[i].owner = 0;
			return;
		}
		else {
			Mutex[i].lockCount = 1;
			Mutex[i].owner = p->p;

			p->inheritedPy = Cp->inheritedPy;
			p->state = READY;

			Cp->inheritedPy = Cp->py;

			Cp->state = READY;

			enqueueRQ(&p, &ReadyQueue, &RQCount);
		}
	}
	else if (Mutex[i].lockCount > 1) {
		Mutex[i].lockCount--;
	}
	else {
		volatile PD* p = dequeueWQ(&WaitingQueue, &WQCount, m);

		if(p == NULL){
			Mutex[i].state = FREE;
			Mutex[i].lockCount = 0;
			Mutex[i].owner = 0;
			Cp->inheritedPy = Cp->py;

			// Turn on pin for newly running task
			if (Cp->p <= 1) {
				enable_LED(PORTL2);
			}
			else if (Cp->p == 2) {
				enable_LED(PORTL5);
			}
			else if (Cp->p == 3) {
				enable_LED(PORTL6);
			}
		}
		else {
			Mutex[i].lockCount = 1;
			Mutex[i].owner = p->p;

			p->inheritedPy = Cp->inheritedPy;
			p->state = READY;

			Cp->inheritedPy = Cp->py;

			Cp->state = READY;

			enqueueRQ(&p, &ReadyQueue, &RQCount);
			enqueueRQ(&Cp, &ReadyQueue, &RQCount);
			Dispatch();
		}
	}
}

EVENT Kernel_Init_Event_At(volatile EVT *e) {
	e->e = Events;
	e->state = UNSIGNALLED;
	e->p = NULL;

	Events++;

	return e->e;
}

static EVENT Kernel_Init_Event() {
	int x;

	if (Events == MAXEVENT) return; // Too many mutexes!

	// find a Disabled mutex that we can use
	for (x = 0; x < MAXEVENT; x++) {
		if (Event[x].state == INACTIVE) break;
	}

	unsigned int e = Kernel_Init_Event_At( &(Event[x]) );

	return e;
}

static unsigned int Kernel_Wait_Event() {
	int i;
	unsigned int e = Cp->eSend;

	for (i = 0; i < MAXEVENT; i++) {
		if (Event[i].e == e) break;
	}

	if (i >= MAXEVENT) {
		return 0;
	}

	if (Event[i].p == NULL) {
		if (Event[i].state == SIGNALLED) {
			Event[i].state = UNSIGNALLED;
			return 0;
		}
		else {
			Cp->eWait = e;
			Event[i].p = Cp->p;
			return 1;
		}
	}

	return 0;
}

static void Kernel_Signal_Event() {
	int i, j;
	unsigned int e = Cp->eSend;

	for (i = 0; i < MAXEVENT; i++) {
		if (Event[i].e == e) break;
	}

	if (i >= MAXEVENT) {
		return;
	}

	for(j = 0; j < MAXTHREAD; j++) {
		if (Process[j].eWait == e) break;
	}

	if (j >= MAXTHREAD) {
		Event[i].state = SIGNALLED;
	}
	else {
		Process[j].state = READY;
		Process[j].eWait = 99;

		Event[i].p = NULL;

		if ((Process[j].inheritedPy < Cp->inheritedPy) && (Process[j].suspended == 0)) {
			Cp->state = READY;
			enqueueRQ(&Cp, &ReadyQueue, &RQCount);
			Dispatch();
		}
	}
}

/**
  * This internal kernel function is a part of the "scheduler". It chooses the 
  * next task to run, i.e., Cp.
  */
static void Dispatch() {
	 /* find the next READY task
	   * Note: if there is no READY task, then this will loop forever!.
	   */

	Cp = dequeueRQ(&ReadyQueue, &RQCount);

	if (Cp == NULL) {
		OS_Abort();
	}

	CurrentSp = Cp->sp;
	Cp->state = RUNNING;

	// Turn on pin for newly running task
	if (Cp->p <= 1) {
		enable_LED(PORTL2);
	}
	else if (Cp->p == 2) {
		enable_LED(PORTL5);
	}
	else if (Cp->p == 3) {
		enable_LED(PORTL6);
	}
}

/**
  * This internal kernel function is the "main" driving loop of this full-served
  * model architecture. Basically, on OS_Start(), the kernel repeatedly
  * requests the next user task's next system call and then invokes the
  * corresponding kernel function on its behalf.
  *
  * This is the main loop of our kernel, called by OS_Start().
  */
static void Next_Kernel_Request() {
	Dispatch();  /* select a new task to run */

	unsigned int mutex_is_locked;
	unsigned int resumed;
	unsigned int waiting;

	while(1) {
		Cp->request = NONE; /* clear its request */

		/* activate this newly selected task */
		CurrentSp = Cp->sp;

		Exit_Kernel();    /* or CSwitch() */

		disable_LED(PORTL2);
		disable_LED(PORTL5);
		disable_LED(PORTL6);

		/* if this task makes a system call, it will return to here! */

		/* save the Cp's stack pointer */
		Cp->sp = CurrentSp;

		switch(Cp->request){
		case CREATE:
			Cp->response = Kernel_Create_Task( Cp->code, Cp->py, Cp->arg );
			break;
		case NEXT:
		case NONE:
			/* NONE could be caused by a timer interrupt */
			Cp->state = READY;
			enqueueRQ(&Cp, &ReadyQueue, &RQCount);
			Dispatch();
			break;
		case SLEEP:
			Cp->state = SLEEPING;
			enqueueSQ(&Cp, &SleepQueue, &SQCount);
			Dispatch();
			break;
		case SUSPEND:
			Kernel_Suspend_Task();
			if(Cp->suspended) {
				Cp->state = READY;
				enqueueRQ(&Cp, &ReadyQueue, &RQCount);
				Dispatch();
			}
			break;
		case RESUME:
			resumed = Kernel_Resume_Task();
			if(resumed){
				Cp->state = READY;
				enqueueRQ(&Cp, &ReadyQueue, &RQCount);
				Dispatch();
			}
			break;
		case TERMINATE:
			/* deallocate all resources used by this task */
			Kernel_Terminate_Task();
			Dispatch();
			break;
		case MUTEX_INIT:
			Cp->response = Kernel_Init_Mutex();
			break;
		case MUTEX_LOCK:
			mutex_is_locked = Kernel_Lock_Mutex();
			if (!mutex_is_locked) {
				Dispatch();
			}
			break;
		case MUTEX_UNLOCK:
			Kernel_Unlock_Mutex();
            break;
        case EVENT_INIT:
        	Cp->response = Kernel_Init_Event();
        	break;
        case EVENT_WAIT:
        	waiting = Kernel_Wait_Event();
        	if (waiting) {
				Cp->state = WAITING;
        		enqueueRQ(&Cp, &ReadyQueue, &RQCount);
        		Dispatch();
        	}
        	
        	// Turn on pin for newly running task
			if (Cp->p <= 1) {
				enable_LED(PORTL2);
			}
			else if (Cp->p == 2) {
				enable_LED(PORTL5);
			}
			else if (Cp->p == 3) {
				enable_LED(PORTL6);
			}

        	break;
        case EVENT_SIGNAL:
        	Kernel_Signal_Event();
        	break;
		default:
			/* Houston! we have a problem here! */
			break;
		}
	} 
}

/*================
  * RTOS  API  and Stubs
  *================
  */

/**
  * This function initializes the RTOS and must be called before any other
  * system calls.
  */
void OS_Init() {
	int x;

	Tasks = 0;
	KernelActive = 0;
	Mutexes = 0;
	Events = 0;
	pCount = 0;

	//Reminder: Clear the memory for the task on creation.
	for (x = 0; x < MAXTHREAD; x++) {
		memset(&(Process[x]),0,sizeof(PD));
		Process[x].state = DEAD;
		Process[x].eWait = 99;
		Process[x].p = 0;
	}

	for (x = 0; x < MAXMUTEX; x++) {
		memset(&(Mutex[x]),0,sizeof(MTX));
		Mutex[x].state = DISABLED;
	}

	for (x = 0; x < MAXEVENT; x++) {
		memset(&(Event[x]),0,sizeof(EVT));
		Event[x].state = INACTIVE;
	}
}

/**
  * This function starts the RTOS after creating a few tasks.
  */
void OS_Start() {   
	if ( (! KernelActive) && (Tasks > 0)) {
		Disable_Interrupt();
		/* we may have to initialize the interrupt vector for Enter_Kernel() here. */

		/* here we go...  */
		KernelActive = 1;
		Next_Kernel_Request();
		/* NEVER RETURNS!!! */
	}
}

void OS_Abort() {
	exit(0);
}

MUTEX Mutex_Init() {
	if(KernelActive) {
		Disable_Interrupt();
		Cp->request = MUTEX_INIT;
		Enter_Kernel();
		return Cp->response;
	}
}

void Mutex_Lock(MUTEX m) {
	if(KernelActive) {
		Disable_Interrupt();
		Cp->request = MUTEX_LOCK;
		Cp->m = m;
		Enter_Kernel();
	}
	
}

void Mutex_Unlock(MUTEX m) {
	if(KernelActive) {
		Disable_Interrupt();
		Cp->request = MUTEX_UNLOCK;
		Cp->m = m;
		Enter_Kernel();
	}
}

EVENT Event_Init() {
	if(KernelActive) {
		Disable_Interrupt();
		Cp->request = EVENT_INIT;
		Enter_Kernel();
		return Cp->response;
	}
}

void Event_Wait(EVENT e) {
	if(KernelActive) {
		Disable_Interrupt();
		Cp->request = EVENT_WAIT;
		Cp->eSend = e;
		Enter_Kernel();
	}
}

void Event_Signal(EVENT e) {
	if(KernelActive) {
		Disable_Interrupt();
		Cp->request = EVENT_SIGNAL;
		Cp->eSend = e;
		Enter_Kernel();
	}
}

/**
  * 
  */
PID Task_Create( voidfuncptr f, PRIORITY py, int arg){
	unsigned int p;

	if (KernelActive) {
		Disable_Interrupt();
		Cp->request = CREATE;
		Cp->code = f;
		Cp->py = py;
		Cp->arg = arg;
		Enter_Kernel();
		p = Cp->response;
	} else { 
	  /* call the RTOS function directly */
	  p = Kernel_Create_Task( f, py, arg );
	}
	return p;
}

/**
  * The calling task gives up its share of the processor voluntarily.
  */
void Task_Next() {
	if (KernelActive) {
		Disable_Interrupt();
		Cp->request = NEXT;
		Enter_Kernel();
	}
}

void Task_Sleep(TICK t) {
	if (KernelActive) {
		Disable_Interrupt();
		Cp->request = SLEEP;
		unsigned int clockTicks = TCNT3/625;
		Cp->wakeTickOverflow = tickOverflowCount + ((t + clockTicks) / 100);
		Cp->wakeTick = (t + clockTicks) % 100;
		Enter_Kernel();
	}
}

void Task_Suspend(PID p) {
	if (KernelActive) {
		Disable_Interrupt();
		Cp->request = SUSPEND;
		Cp->pidAction = p;
		Enter_Kernel();
	}
}

void Task_Resume(PID p) {
	if (KernelActive) {
		Disable_Interrupt();
		Cp->request = RESUME;
		Cp->pidAction = p;
		Enter_Kernel();
	}
}

/**
  * The calling task terminates itself.
  */
void Task_Terminate() {
	if (KernelActive) {
		Disable_Interrupt();
		Cp -> request = TERMINATE;
		Enter_Kernel();
		/* never returns here! */
	}
}

int Task_GetArg(PID p) {
	return (Cp->arg);
}

void setup() {
	// pin 47
	init_LED_PORTL_pin2();

	// pin 43
	init_LED_PORTL_pin6();

	// pin 44
	init_LED_PORTL_pin5();

	// pin 49
	init_LED_PORTL_pin0();

	// pin 48
	init_LED_PORTL_pin1();

	// initialize Timer1 16 bit timer
	Disable_Interrupt();

	// Timer 1
	TCCR1A = 0;                 // Set TCCR1A register to 0
	TCCR1B = 0;                 // Set TCCR1B register to 0

	TCNT1 = 0;                  // Initialize counter to 0

	OCR1A = 624;                // Compare match register (TOP comparison value) [(16MHz/(100Hz*8)] - 1

	TCCR1B |= (1 << WGM12);     // Turns on CTC mode (TOP is now OCR1A)

	TCCR1B |= (1 << CS12);      // Prescaler 256

	TIMSK1 |= (1 << OCIE1A);    // Enable timer compare interrupt

	// Timer 3
	TCCR3A = 0;                 // Set TCCR0A register to 0
	TCCR3B = 0;                 // Set TCCR0B register to 0

	TCNT3 = 0;                  // Initialize counter to 0

	OCR3A = 62499;                // Compare match register (TOP comparison value) [(16MHz/(100Hz*8)] - 1

	TCCR3B |= (1 << WGM32);     // Turns on CTC mode (TOP is now OCR1A)

	TCCR3B |= (1 << CS32);      // Prescaler 1024

	TIMSK3 = (1 << OCIE3A);

	Enable_Interrupt();
}

ISR(TIMER1_COMPA_vect) {

	volatile int i;

	for (i = SQCount-1; i >= 0; i--) {
		if ((SleepQueue[i]->wakeTickOverflow <= tickOverflowCount) && (SleepQueue[i]->wakeTick <= (TCNT3/625))) {
			volatile PD *p = dequeue(&SleepQueue, &SQCount);
			p->state = READY;
			enqueueRQ(&p, &ReadyQueue, &RQCount);
		}
		else {
			break;
		}
	}

	Task_Next();
}

ISR(TIMER3_COMPA_vect) {
	tickOverflowCount += 1;
}

/**
  * This function creates two cooperative tasks, "Ping" and "Pong". Both
  * will run forever.
  */
void main() {
	setup();

	OS_Init();
	Task_Create(a_main, 0, 1);
	OS_Start();
}

