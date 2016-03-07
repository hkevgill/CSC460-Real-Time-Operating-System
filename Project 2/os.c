#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include "LED_Test.h"
#include "os.h"

//Comment out the following line to remove debugging code from compiled version.
#define DEBUG

typedef void (*voidfuncptr) (void);      /* pointer to void f(void) */ 

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

#define Disable_Interrupt()     asm volatile ("cli"::)
#define Enable_Interrupt()      asm volatile ("sei"::)

/**
  *  This is the set of states that a task can be in at any given time.
  */
typedef enum process_states { 
    DEAD = 0, 
    READY,
    RUNNING,
    SLEEPING,
    SUSPENDED
} PROCESS_STATES;

/**
  * This is the set of kernel requests, i.e., a request code for each system call.
  */
typedef enum kernel_request_type {
    NONE = 0,
    CREATE,
    NEXT,
    SLEEP,
    TERMINATE
} KERNEL_REQUEST_TYPE;

/**
  * Each task is represented by a process descriptor, which contains all
  * relevant information about this task. For convenience, we also store
  * the task's stack, i.e., its workspace, in here.
  */
typedef struct ProcessDescriptor {
    PID p;
    unsigned char *sp;   /* stack pointer into the "workSpace" */
    unsigned char workSpace[WORKSPACE]; 
    PROCESS_STATES state;
    PRIORITY py;
    int arg;
    voidfuncptr  code;   /* function to be executed as a task */
    KERNEL_REQUEST_TYPE request;
    TICK wakeTickOverflow;
    TICK wakeTick;
} PD;

/**
  * This table contains ALL process descriptors. It doesn't matter what
  * state a task is in.
  */
static PD Process[MAXTHREAD];

volatile PD *ReadyQueue[MAXTHREAD];
volatile int RQCount = 0;

volatile PD *SleepQueue[MAXTHREAD];
volatile int SQCount = 0;
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

/** index to next task to run */
volatile static unsigned int NextP;  

/** 1 if kernel has been started; 0 otherwise. */
volatile static unsigned int KernelActive;  

/** number of tasks created so far */
volatile static unsigned int Tasks;  

// Next process id
volatile unsigned int pCount = 0;

// Global tick overflow count
volatile unsigned int tickOverflowCount = 0;

/*
 *  Checks if queue is full
 */
volatile int isFull(volatile int *QCount) {
	return *QCount == MAXTHREAD - 1;
}

/*
 *  Checks if queue is empty, READY QUEUE SHOULD NEVER BE EMPTY
 */
volatile int isEmpty(volatile int *QCount) {
	return *QCount == 0;
}

/*
 *  Insert into the queue sorted by priority
 */
void enqueue(volatile PD **p, volatile PD **Queue, volatile int *QCount) {
    if(isFull(QCount)) {
        return;
    }

    int i = (*QCount) - 1;

    volatile PD *new = *p;

    volatile PD *temp = Queue[i];

    while(i >= 0 && (new->py >= temp->py)) {
        Queue[i+1] = Queue[i];
        i--;
        temp = Queue[i];
    }

    Queue[i+1] = *p;
    (*QCount)++;
}

/*
 *  Return the first element of the queue
 */
volatile PD *dequeue(volatile PD **Queue, volatile int *QCount) {

	if(isEmpty(QCount)) {
        return;
    }

    volatile PD *result = (Queue[(*QCount)-1]);
    (*QCount)--;

    return result;
}

/**
 * When creating a new task, it is important to initialize its stack just like
 * it has called "Enter_Kernel()"; so that when we switch to it later, we
 * can just restore its execution context on its stack.
 * (See file "cswitch.S" for details.)
 */
void Kernel_Create_Task_At( volatile PD *p, voidfuncptr f, PRIORITY py, int arg ) {   
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
    pCount++;
    p->py = py;
    p->arg = arg;

    /*----END of NEW CODE----*/

    p->state = READY;

    // Add to ready queue
    enqueue(&p, &ReadyQueue, &RQCount);

}

/**
  *  Create a new task
  */
static void Kernel_Create_Task( voidfuncptr f, PRIORITY py, int arg ) {
    int x;

    if (Tasks == MAXTHREAD) return;  /* Too many task! */

    /* find a DEAD PD that we can use  */
    for (x = 0; x < MAXTHREAD; x++) {
        if (Process[x].state == DEAD) break;
    }

    ++Tasks;
    Kernel_Create_Task_At( &(Process[x]), f, py, arg );

}

/**
  * This internal kernel function is a part of the "scheduler". It chooses the 
  * next task to run, i.e., Cp.
  */
static void Dispatch() {
     /* find the next READY task
       * Note: if there is no READY task, then this will loop forever!.
       */

    // Cp = dequeueRQ();
    Cp = dequeue(&ReadyQueue, &RQCount);

    CurrentSp = Cp->sp;
    Cp->state = RUNNING;
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

    while(1) {
        Cp->request = NONE; /* clear its request */

        /* activate this newly selected task */
        CurrentSp = Cp->sp;

        Exit_Kernel();    /* or CSwitch() */

        /* if this task makes a system call, it will return to here! */

        /* save the Cp's stack pointer */
        Cp->sp = CurrentSp;

        switch(Cp->request){
        case CREATE:
            Kernel_Create_Task( Cp->code, Cp->py, Cp->arg );
            break;
        case NEXT:
        case NONE:
            /* NONE could be caused by a timer interrupt */
            Cp->state = READY;
            enqueue(&Cp, &ReadyQueue, &RQCount);
            Dispatch();
            break;
        case SLEEP:
            Cp->state = SLEEPING;
            enqueue(&Cp, &SleepQueue, &SQCount);
            Dispatch();
            break;
        case TERMINATE:
            /* deallocate all resources used by this task */
            Cp->state = DEAD;
            Dispatch();
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
    NextP = 0;
    //Reminder: Clear the memory for the task on creation.
    for (x = 0; x < MAXTHREAD; x++) {
        memset(&(Process[x]),0,sizeof(PD));
        Process[x].state = DEAD;
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

/**
  * For this example, we only support cooperatively multitasking, i.e.,
  * each task gives up its share of the processor voluntarily by calling
  * Task_Next().
  */
PID Task_Create( voidfuncptr f, PRIORITY py, int arg){
    if (KernelActive) {
        Disable_Interrupt();
        Cp->request = CREATE;
        Cp->code = f;
        Cp->py = py;
        Cp->arg = arg;
        Enter_Kernel();
    } else { 
      /* call the RTOS function directly */
      Kernel_Create_Task( f, py, arg );
    }
    // return PID
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
	int i;
	for (i = 0; i < MAXTHREAD; i++){
		if (Process[i].p == p) {
			return Process[i].arg;
		}
	}
	return -1;
}

/*============
  * A Simple Test 
  *============
  */

/**
  * A cooperative "Ping" task.
  * Added testing code for LEDs.
  */
void Ping() {
    int  x ;
    for(;;){
        // enable_LED(PORTL6);
        // disable_LED(PORTL2);

        for( x=0; x < 32000; ++x );   /* do nothing */
        for( x=0; x < 32000; ++x );   /* do nothing */
        for( x=0; x < 32000; ++x );   /* do nothing */

        /* printf( "*" );  */
        Task_Sleep(50);
    }
}

/**
  * A cooperative "Pong" task.
  * Added testing code for LEDs.
  */
void Pong() {
    int  x;
    for(;;) {
        // enable_LED(PORTL2);
        // disable_LED(PORTL6);

        for( x=0; x < 32000; ++x );   /* do nothing */
        for( x=0; x < 32000; ++x );   /* do nothing */
        for( x=0; x < 32000; ++x );   /* do nothing */

        /* printf( "." );  */
        Task_Sleep(50);

    }
}

void setup() {
    // pin 47
    init_LED_PORTL_pin2();

    // pin 43
    init_LED_PORTL_pin6();

    // pin 44
    init_LED_PORTL_pin5();

    // initialize Timer1 16 bit timer
    Disable_Interrupt();

    // Timer 1
    TCCR1A = 0;                 // Set TCCR1A register to 0
    TCCR1B = 0;                 // Set TCCR1B register to 0

    TCNT1 = 0;                  // Initialize counter to 0

    OCR1A = 6249;                // Compare match register (TOP comparison value) [(16MHz/(100Hz*8)] - 1

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

    if (isEmpty(&SQCount)) {
        enable_LED(PORTL6);
        return;
    }
    else {
        if ((SleepQueue[0]->wakeTickOverflow <= tickOverflowCount) && (SleepQueue[0]->wakeTick <= (TCNT3/625))) {
            toggle_LED(PORTL2);
            PD *p = dequeue(&SleepQueue, &SQCount);
            enqueue(&p, &ReadyQueue, &RQCount);
        }
    }
    Task_Next();
}

ISR(TIMER3_COMPA_vect) {
    tickOverflowCount += 1;
}

void idle() {
    for(;;) {
    }
}

void a_main() {
    Task_Create(Pong, 8, 1);
    Task_Create(Ping, 8, 1);
    Task_Create(idle, 10, 1);

    Task_Terminate();
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

