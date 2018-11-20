#ifndef slic3r_GUI_BackgroundSlicingProcess_hpp_
#define slic3r_GUI_BackgroundSlicingProcess_hpp_

#include <string>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Print.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class GCodePreviewData;
class Model;
class SLAPrint;

class SlicingStatusEvent : public wxEvent
{
public:
	SlicingStatusEvent(wxEventType eventType, int winid, const PrintBase::Status &status) :
		wxEvent(winid, eventType), status(std::move(status)) {}
	virtual wxEvent *Clone() const { return new SlicingStatusEvent(*this); }

	PrintBase::Status 	status;
};

wxDEFINE_EVENT(EVT_SLICING_UPDATE, SlicingStatusEvent);

// Print step IDs for keeping track of the print state.
enum BackgroundSlicingProcessStep {
    bspsGCodeFinalize, bspsCount,
};

// Support for the GUI background processing (Slicing and G-code generation).
// As of now this class is not declared in Slic3r::GUI due to the Perl bindings limits.
class BackgroundSlicingProcess
{
public:
	BackgroundSlicingProcess();
	// Stop the background processing and finalize the bacgkround processing thread, remove temp files.
	~BackgroundSlicingProcess();

	void set_fff_print(Print *print) { m_fff_print = print; }
	void set_sla_print(SLAPrint *print) { m_sla_print = print; }
	void set_gcode_preview_data(GCodePreviewData *gpd) { m_gcode_preview_data = gpd; }
	// The following wxCommandEvent will be sent to the UI thread / Platter window, when the slicing is finished
	// and the background processing will transition into G-code export.
	// The wxCommandEvent is sent to the UI thread asynchronously without waiting for the event to be processed.
	void set_slicing_completed_event(int event_id) { m_event_slicing_completed_id = event_id; }
	// The following wxCommandEvent will be sent to the UI thread / Platter window, when the G-code export is finished.
	// The wxCommandEvent is sent to the UI thread asynchronously without waiting for the event to be processed.
	void set_finished_event(int event_id) { m_event_finished_id = event_id; }

	// Activate either m_fff_print or m_sla_print.
	void select_technology(PrinterTechnology tech);
	// Start the background processing. Returns false if the background processing was already running.
	bool start();
	// Cancel the background processing. Returns false if the background processing was not running.
	// A stopped background processing may be restarted with start().
	bool stop();
	// Cancel the background processing and reset the print. Returns false if the background processing was not running.
	// Useful when the Model or configuration is being changed drastically.
	bool reset();

	// Apply config over the print. Returns false, if the new config values caused any of the already
	// processed steps to be invalidated, therefore the task will need to be restarted.
	Print::ApplyStatus apply(const Model &model, const DynamicPrintConfig &config);
	// After calling apply, the empty() call will report whether there is anything to slice.
	bool 		empty() const;
	// Validate the print. Returns an empty string if valid, returns an error message if invalid.
	// Call validate before calling start().
	std::string validate();

	// Set the export path of the G-code.
	// Once the path is set, the G-code 
	void schedule_export(const std::string &path);
	// Clear m_export_path.
	void reset_export();
	// Once the G-code export is scheduled, the apply() methods will do nothing.
	bool is_export_scheduled() const { return ! m_export_path.empty(); }

	enum State {
		// m_thread  is not running yet, or it did not reach the STATE_IDLE yet (it does not wait on the condition yet).
		STATE_INITIAL = 0,
		// m_thread is waiting for the task to execute.
		STATE_IDLE,
		STATE_STARTED,
		// m_thread is executing a task.
		STATE_RUNNING,
		// m_thread finished executing a task, and it is waiting until the UI thread picks up the results.
		STATE_FINISHED,
		// m_thread finished executing a task, the task has been canceled by the UI thread, therefore the UI thread will not be notified.
		STATE_CANCELED,
		// m_thread exited the loop and it is going to finish. The UI thread should join on m_thread.
		STATE_EXIT,
		STATE_EXITED,
	};
	State 	state() 	const { return m_state; }
	bool    idle() 		const { return m_state == STATE_IDLE; }
	bool    running() 	const { return m_state == STATE_STARTED || m_state == STATE_RUNNING || m_state == STATE_FINISHED || m_state == STATE_CANCELED; }

private:
	void 	thread_proc();
	void 	thread_proc_safe();
	void 	join_background_thread();
	// To be called by Print::apply() through the Print::m_cancel_callback to stop the background
	// processing before changing any data of running or finalized milestones.
	// This function shall not trigger any UI update through the wxWidgets event.
	void	stop_internal();

	// Helper to wrap the FFF slicing & G-code generation.
	void	process_fff();

    // Temporary: for mimicking the fff file export behavior with the raster output
    void	process_sla();

	// Currently active print. It is one of m_fff_print and m_sla_print.
	PrintBase				   *m_print 			 = nullptr;
	// Non-owned pointers to Print instances.
	Print 					   *m_fff_print 		 = nullptr;
	SLAPrint 				   *m_sla_print			 = nullptr;
	// Data structure, to which the G-code export writes its annotations.
	GCodePreviewData 		   *m_gcode_preview_data = nullptr;
	// Temporary G-code, there is one defined for the BackgroundSlicingProcess, differentiated from the other processes by a process ID.
	std::string 				m_temp_output_path;
	// Output path provided by the user. The output path may be set even if the slicing is running,
	// but once set, it cannot be re-set.
	std::string 				m_export_path;
	// Thread, on which the background processing is executed. The thread will always be present
	// and ready to execute the slicing process.
	std::thread		 			m_thread;
	// Mutex and condition variable to synchronize m_thread with the UI thread.
	std::mutex 		 			m_mutex;
	std::condition_variable		m_condition;
	State 						m_state = STATE_INITIAL;

    PrintState<BackgroundSlicingProcessStep, bspsCount>   	m_step_state;
    mutable tbb::mutex                      				m_step_state_mutex;
	bool                set_step_started(BackgroundSlicingProcessStep step);
	void                set_step_done(BackgroundSlicingProcessStep step);
	bool 				is_step_done(BackgroundSlicingProcessStep step) const;
	bool                invalidate_step(BackgroundSlicingProcessStep step);
    bool                invalidate_all_steps();
    // If the background processing stop was requested, throw CanceledException.
    void                throw_if_canceled() const { if (m_print->canceled()) throw CanceledException(); }

	// wxWidgets command ID to be sent to the platter to inform that the slicing is finished, and the G-code export will continue.
	int 						m_event_slicing_completed_id 	= 0;
	// wxWidgets command ID to be sent to the platter to inform that the task finished.
	int 						m_event_finished_id  			= 0;
};

}; // namespace Slic3r

#endif /* slic3r_GUI_BackgroundSlicingProcess_hpp_ */
