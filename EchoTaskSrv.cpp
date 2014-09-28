/*
 * To make the executable, please use:
 * $ c++ Program.cpp -I$ACE_ROOT -L$ACE_ROOT -lACE -lpthread
 *
 * $ACE_ROOT should point your ACE root directory
 *
 * Versions of software:
 * COMPILER: c++ (Ubuntu/Linaro 4.6.3-1ubuntu5) 4.6.3 (gcc)
 * ACE library: 6.2.7
 *
 */

#include <assert.h>
#include "ace/Acceptor.h"
#include "ace/Svc_Handler.h"
#include "ace/Svc_Handler.h"
#include "ace/SOCK_Stream.h"
#include "ace/OS_NS_string.h"

#include "ace/Log_Msg.h"
#include "ace/INET_Addr.h"
#include "ace/SOCK_Acceptor.h"
#include "ace/Reactor.h"
#include "ace/Acceptor.h"

#include "ace/Message_Block.h"
#include "ace/SOCK_Stream.h"
#include "ace/Svc_Handler.h"

#include <string.h>

// Set the port number here
static const u_short ECHO_SERVER_PORT = ACE_DEFAULT_SERVER_PORT;

// Amount of time to wait for the client to send data
static const int TIMEOUT_SECS = 30;

/**
 * @ class Echo_Command
 *
 * @brief this class is used by the Asynchronous Service Layer
 * (Echo_Svc_Handler) to enqueue a command in the Queuing Layer
 * (ACE_Message_Queue) that is then executed by a thread in the
 * Synchronous Service Layer (Echo_Task).
 *
 * This implements the Command pattern
 */
template <typename PEER_STREAM>
class Echo_Command : public ACE_Message_Block
{
public:
	Echo_Command (ACE_Svc_Handler <PEER_STREAM, ACE_NULL_SYNCH> *svc_handler,
			      ACE_Message_Block *client_input)
	   : svc_handler_ (svc_handler)
	{
		// Attach the client_input on the message continuation chain.
		this->init(ACE_DEFAULT_MAX_SOCKET_BUFSIZ);
		this->copy(client_input->rd_ptr(), client_input->length());

		client_input->release();

	}

	// Accessor for the service handler pointer.
	ACE_Svc_Handler<PEER_STREAM, ACE_NULL_SYNCH> *svc_handler (void)
	{
		return svc_handler_;
	}

	// When an Echo_Command is executed it echoes the data back to the
	// client after first prepending the thread id.
	virtual int execute (void)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Command::execute\n"));
		//send_cnt = this->peer().send(buf, recv_cnt);
		this->svc_handler_->peer().send(this->rd_ptr(), this->length());

		return 0;
	}

private:
	// Pointer to our service handler.
	ACE_Svc_Handler <PEER_STREAM, ACE_NULL_SYNCH> *svc_handler_;

};

/**
 * @class Echo_Task
 *
 * @brief Contains a thread pool that dequeues client input from a
 * synchronized message queue and them echoes the data received from
 * client back to the client using blocking I/O.
 *
 * This class plays the Synchronous service Layer and Queueing Layer
 * role in the Half-Sync/Half-Async patter.
 */
template <typename PEER_STREAM>
class Echo_Task : public ACE_Task <ACE_MT_SYNCH>
{
public:
	// Size of the thread pool used by the Synchronous Service Layer.
	static const int THREAD_POOL_SIZE = 4;

	// Set the "high water mark" to 20000K
	static const int HIGH_WATER_MARK = 1024 * 20000;

	// Set the HIGH_WATER_MARK of the underlying ACE_Message_Queue and
	// activate this task to run as an active object with a thread
	// pool
	virtual int open(void * = 0)
	{
		// Set the high water_mark, which limits the amount of data
		// that will be used to buffer client input pending successfully
		// echoing back to the client.
		ACE_DEBUG((LM_DEBUG, "Echo_Task::open\n"));

		msg_queue ()-> high_water_mark (HIGH_WATER_MARK);

		// Activate the task to run a thread pool.
		if (activate (THR_DETACHED,
		              THREAD_POOL_SIZE) == -1)
		   ACE_ERROR_RETURN ((LM_ERROR,
		                      ACE_TEXT("(%P|%t) activate failed\n")),
		                      -1);
		return 0;
	}

	// This hook method runs in each thread of control in the thread pool.
	virtual int svc (void)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Task::svc is running\n"));
		// Block until there is a message available on the queue.
		for (ACE_Message_Block *message_block; getq (message_block) != -1;)
		{
			// Get and execute the echo_command to echo the data back to
			// the client
			ACE_DEBUG((LM_DEBUG, "Echo_Task::svc: thread %ul is waiting for messages\n", ACE_OS::thr_self()));
			Echo_Command<ACE_SOCK_Stream> *echo_command =
					reinterpret_cast<Echo_Command<ACE_SOCK_Stream> *>( message_block );

			echo_command->execute();

		}
		return 0;
	}

	// Enqueue the client input in the ACE_Message_Queue.
	virtual int put (ACE_Message_Block *client_input,
					 ACE_Time_Value *timeout = 0)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Task::put\n"));

		return putq (client_input, timeout);
	}
};

/**
 * @class Echo_Svc_Handler
 *
 * @brief Echoes data received from the client back to the client using
 * nonblocking I/O
 *
 * The class plays the concrete event handler role in the Reactor
 * pattern and the service handler role in the
 * Acceptor/Connector pattern.
 */
template <typename PEER_STREAM>
class Echo_Svc_Handler : public ACE_Svc_Handler<PEER_STREAM, ACE_NULL_SYNCH>
	// ACE_SOCK_Stream plays the role of a wrapper facade in the
	// Wrapper Facade pattern.
{
public:
	enum Client_Input_State
	{
		// We need to use the reactorto wait for more data from the
		/// client.
		WAIT_TO_RECV_MORE_CLIENT_INPUT,

		// We have existing message fragment(s) that must be processed
		// before waiting the reactor again
		MUST_PROCESS_FRAGMENT_DATA
	};

	// This constructor takes a default parameters of 0 so that it will
	// work properly with the make_svc_handler() method in the
	// Echo_Acceptor.

	Echo_Svc_Handler (ACE_Thread_Manager* thr_mgr = 0,
	                  ACE_Reactor *reactor = 0,
	                  Echo_Task<PEER_STREAM>*echo_task = 0)
	   : ACE_Svc_Handler <PEER_STREAM, ACE_NULL_SYNCH> (thr_mgr, 0 , reactor),
	     client_input_state_ (WAIT_TO_RECV_MORE_CLIENT_INPUT),
	     echo_task_ (echo_task),
	     client_input_ (0),
	     current_fragment_ (0)
	{
	}

	// Hook method called by the ACE_Acceptor to activate the service
	// handler
	virtual int open (void *acceptor)
	{
		// Schedule a timeout to guard against clients that connect by
		// don't send data

		ACE_OS::printf("Echo_Svc_Handler::open schedule timer\n");

		if (this->reactor()->schedule_timer (this,
											0,
											ACE_Time_Value (TIMEOUT_SECS)) == -1)
					ACE_ERROR_RETURN ((LM_ERROR,
								       ACE_TEXT ("(%P|%t) schedule timer failed\n")),
								       -1);

		// Forward to the parent class's open() method, which registers
		// this service handler for reactive dispatch when data arrives.
		if ( this->reactor ()->register_handler (this, ACE_Event_Handler::READ_MASK) == -1)
			ACE_ERROR_RETURN((LM_ERROR,
					          "(%P|%t) can't register with reactor\n"),
							  -1);

		return 0;
	}

	// Close down the event handler.
	virtual int handle_close (ACE_HANDLE h,
	                          ACE_Reactor_Mask m)
	{
		if (this->reactor ()->cancel_timer (this) == -1)
			ACE_ERROR((LM_ERROR,
			           ACE_TEXT ("(%P|%t) cancel_timer failed\n")));

		ACE_OS::printf("Echo_Svc_Handler::handle_close: close down the event handler\n");

		return ACE_Svc_Handler <ACE_SOCK_Stream, ACE_NULL_SYNCH>::handle_close (h,m);

	}

	// This hook method is used to shutdown the service handler if the
	// client doesn't send data for several seconds.
	virtual int handle_timeout (ACE_Time_Value const &,
	                            void const*)
	{
		// Instruct the reactor to remove this service handler and shut it
		// down
		ACE_OS::printf("Echo_Svc_Handler::handle_timeout: remove the service handler and shut it\n");

		this->reactor () -> remove_handler (this, ACE_Event_Handler::READ_MASK);

		return 0;
	}

	// This hook method is dispatched by ACE Reactor when data
	// shows up from a client.
	virtual int handle_input (ACE_HANDLE)
	{
		// Reschedule the connection timer.
		if ( reschedule_timer() == -1 )
			ACE_ERROR_RETURN ((LM_ERROR,
			                   ACE_TEXT ("(%P|%t_ %p\n"),
			                   ACE_TEXT ("reschedule_timer_failed")),
			                   -1);

		switch (client_input_state_)
		{
			case WAIT_TO_RECV_MORE_CLIENT_INPUT:
				// Receive input from the client and process it.
				ACE_DEBUG((LM_DEBUG, "Echo_Svc_Handler::handle_input: WAIT_TO_RECV_MORE_CLIENT_INPUT\n"));
				return recv_client_input ();

			case MUST_PROCESS_FRAGMENT_DATA:
				// Process the input from existing fragment data to see if we
				// get a complete message
				ACE_DEBUG((LM_DEBUG,"Echo_Svc_Handler::handle_input: MUST_PROCESS_FRAGMENT_DATA\n"));
				return process_input ();
		}

		return 0;
	}

	// receive input from the client and process it.
	int recv_client_input (void)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Svc_Handler::recv_client_input is running\n"));

		if (! recv_message())
		{
   	        client_input_state_ = MUST_PROCESS_FRAGMENT_DATA;
   	        return process_input();
		}

	    return 0;
	}

	// Process the input that was received from a client (either via a
	// call to recv_message() or from existing fragment data to see if
	// we get a complete message
	int process_input (void)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Svc_Handler::process_input\n"));

		if (have_complete_message(current_fragment_))
			return pass_to_synchronous_service_layer( current_fragment_ );
		else
			client_input_state_ = WAIT_TO_RECV_MORE_CLIENT_INPUT;

		return 0;
	}

	// Receive the next chunk of data from the client.
	int recv_message (void)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Svc_Handler::recv_message\n"));

		char buf[ACE_DEFAULT_MAX_SOCKET_BUFSIZ];
		ssize_t recv_cnt;

	    // Read the data sent by the client into a buffer
	    recv_cnt = this->peer().recv(buf, sizeof(buf) );

	    // Evaluate the number of bytes transferred.
	    switch (recv_cnt)
	    {
	    case -1:
	    // An error occurred before the entire amount was transferred.
	        ACE_ERROR_RETURN((LM_ERROR,
	            "(%P|%t) %p bad read\n",
	            "client logger"),
	            -1);
	        break;

	    case 0:
	    // EOF, i.e., the peer closed the connection.
	        ACE_ERROR_RETURN((LM_ERROR,
	            "(%P|%t) closing log daemon (fd = %d)\n",
	            this->get_handle()),
	            -1);
	        break;
	    default:
   			buf[recv_cnt] = 0;
   	        ACE_DEBUG((LM_DEBUG,
   	            "Echo_Svc_Handler::recv_client_input: received from client: %s",
   	            buf));

   	        // Put data into the current_fragment_
   			if (current_fragment_ == 0)
   				ACE_NEW_RETURN (current_fragment_, ACE_Message_Block (ACE_DEFAULT_MAX_SOCKET_BUFSIZ), -1);

   			current_fragment_->copy(buf, recv_cnt);
	    }

		return 0;
	}

	// Returns true if we have a complete message (i.e., one that is
	// terminated with a newline and false if we did not. If the
	// method returns true then complete_message points to the message
	// that was received. this method handles the fragmentation that
	// occurs if a newline doesn't appear at the end of the client
	bool have_complete_message (ACE_Message_Block *&complete_message)
	{
		ACE_DEBUG((LM_DEBUG, "Echo_Svc_Handler::have_complete_message\n"));

		if (current_fragment_ == 0)
			return false;

		if (! contains_endofline(current_fragment_->rd_ptr(),
				               (ACE_OS::strlen (current_fragment_->rd_ptr ())+1)) )
			return false;

		return true; // complete message
	}

	// Called by process_input() to pass a complete_message (i.e.,
	// newline-terminated) up to the Synchronous service Layer via the
	// Queueuing Layer
	int pass_to_synchronous_service_layer (ACE_Message_Block *complete_message)
	{
		// create Echo_Command that will echo the input back to the client.
		Echo_Command<ACE_SOCK_Stream> *echo_command =
			new Echo_Command<ACE_SOCK_STREAM> (this, complete_message);

		// Don't receive any more input on this connection until we've
		// echoed the data back to the client.
		suspend_handler();

		// Use the put() hook method to enqueue the echo_command in the
		// Queueuing Layer.
		if (echo_task_->put (echo_command) == -1)
		{
			ACE_ERROR((LM_ERROR,
			           ACE_TEXT ("(%P|%t) %p\n"),
			           ACE_TEXT ("put failed")));
			echo_command->release ();

			return -1;
		} else
			return 0;
	}

	// Reschedule the connection timer.
	int reschedule_timer (void)
	{
		// Cancel the existing timer..
		if (this->reactor () -> cancel_timer(this) == -1)
			ACE_ERROR_RETURN((LM_ERROR,
			                  ACE_TEXT("(%P|%t) cancel timer failed\n")),
			                  -1);
		// .. and reschedule it for TIMEOUT_SECS.
		else if (this->reactor()->schedule_timer (this,
									0,
									ACE_Time_Value (TIMEOUT_SECS)) == -1)
			ACE_ERROR_RETURN ((LM_ERROR,
						       ACE_TEXT ("(%P|%t) schedule timer failed\n")),
						       -1);
		else
			return 0;
	}

	// Suspend the event handler
	int suspend_handler (void)
	{
		return this ->reactor ()-> suspend_handler(this);
	}

	// resume the event handler
	int resume_handler (void)
	{
		return this->reactor ()-> resume_handler(this);
	}

	// Checks to see if the latest client input contains the
	// end of line marker
	char *contains_endofline (char *buffer, size_t length)
	{
		char *nl = static_cast<char *> ( ACE_OS::memchr (buffer, '\n', length) );

		if (nl != 0)
			return nl;

		char *cr = static_cast<char *> (ACE_OS::memchr (buffer, '\n', length));
		if (cr != 0)
		{
			if (cr - buffer < (ssize_t) length && *(cr + 1) == '\n')
				return cr + 1;
			else
				return cr;
		}
		return 0;
	}

private:
	// Keeps track of which state we'are in, which is either
	// WAIT_TO_RECV_MORE_CLIENT_INPUT or MUST_PROCESS_FRAGMENT_DATA
	Client_Input_State client_input_state_;

	// Pointer to the thread pookl that plays role of the
	// synchronous layer in the HSHA pattern.
	Echo_Task<PEER_STREAM> *echo_task_;

	// A pointer to the message fragment(s) received from connected
	// client.
	ACE_Message_Block *client_input_;

	// A pointer to the current message fragment received from the connected client
	ACE_Message_Block *current_fragment_;
};


/** @class Echo_Svc_Handler_Creation_strategy
 *
 * @brief this class is used by the ACE_strategy_acceptor to pass the
 * Echo_Task pointer to the service handler constructor so there is no
 * need to subclass ACE_Acceptor.
 *
 * This class is an example of the Strategy pattern.
 */
 template <typename TASK, typename SVC_HANDLER>
 class Echo_Svc_Handler_Creation_Strategy
 	: public ACE_Creation_Strategy<SVC_HANDLER>
{
public:
	Echo_Svc_Handler_Creation_Strategy (ACE_Reactor *reactor, TASK *task)
		: ACE_Creation_Strategy<SVC_HANDLER> (0, reactor),
		  task_ (task)
	{
	}

	// A factory method that creates a SVC_HANDLER that holds a pointer
	// to the TASK, which contains the thread pool used by the
	// Synchronous Service Layer in the HSHA pattern.
	virtual int make_svc_handler (SVC_HANDLER *&sh)
	{
		sh = new SVC_HANDLER (0, this->reactor_, task_);
		return 0;
	}
private:
	// Pointer to the TASK that contains the thread pool used by the
	// Synchronous Service Layer in the HSHA pattern.
	TASK *task_;
};

// Used by the Asynchronous Service Layer (Echo_Svc_Handler) to
// enqueue a Command in the Queueueing Layer  (ACE_Message_Queue) that is
// then executed by a thread in the Synchronous Service Layer (Echo_Task)
typedef Echo_Task<ACE_SOCK_ACCEPTOR::PEER_STREAM> ECHO_TASK;

// service handler used to pass complete messages that can consist of
// multiple recv calls to an echo task thread pool for further
// processing using the tasks's message queue
typedef Echo_Svc_Handler<ACE_SOCK_ACCEPTOR::PEER_STREAM> ECHO_SVC_HANDLER;

// A simple echo svc handler creation strategy that is passed to the
// acceptor. It helps to pass task pointer to the service
// handlers.
typedef Echo_Svc_Handler_Creation_Strategy<ECHO_TASK,
                                           ECHO_SVC_HANDLER>
                                           ECHO_SVC_HANDLER_CREATION_STRATEGY;

// Configure the ACE_Strategy_acceptor factory that accepts
// connections and creates/activates ECHO_SVC_HANDLERS to process data
// on the connections. It can is passed a handler creation strategy
// from the ECHO_SVC_Handler parameter. It is also configured to use
// ACE socket wrapper facade.

// This class plays role of the concrete acceptor role in the Acceptor/connector pattern. the
// ACE_SOCK_Acceptor plays role of


typedef ACE_Strategy_Acceptor<ECHO_SVC_HANDLER, ACE_SOCK_ACCEPTOR> ECHO_ACCEPTOR;

int ACE_TMAIN (int argc, ACE_TCHAR *argv[])
{
    // Allow us to define a specific listen port by command line or use the default 20002
    ACE_OS::printf("Usage: %s [port-number]\n", argv[0]);
    u_short port = argc < 2 ? ACE_DEFAULT_SERVER_PORT : ACE_OS::atoi(argv[1]);

    // This object plays role of the reactor in the reactor pattern
    ACE_Reactor reactor;

    // The thread pool that plays the role of the Synchronous service
    // layer in the Half-Sync/Half-Async pattern
    ECHO_TASK echo_task;
    echo_task.open();

    // Acceptor-Connector pattern
    ECHO_ACCEPTOR echo_acceptor;

    // Instantiate a creation strategy that's passed to the
    // echo_acceptor
    ECHO_SVC_HANDLER_CREATION_STRATEGY creation_strategy(&reactor, &echo_task);

    // Listen on ECHO_SERVER_PORT and register with the reactor.
    if(echo_acceptor.open(ACE_INET_Addr (ECHO_SERVER_PORT),
    		                              &reactor,
    		                              &creation_strategy) == -1)
    	ACE_ERROR_RETURN ((LM_ERROR,
    			          ACE_TEXT("(%P|%t)%p=n"),
    			          ACE_TEXT ("acceptor open failed")),
    			          1);
    // Run the reactor event loop, which blocks.
    ACE_OS::printf("listening at port %d\n", port);
    reactor.run_reactor_event_loop ();

    return (0);
}
