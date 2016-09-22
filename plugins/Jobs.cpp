////p /src/bedrock/BedrockPlugin_Jobs.cpp
#include <libstuff/libstuff.h>
#include "../BedrockPlugin.h"

#undef SLOGPREFIX
#define SLOGPREFIX "{" << node->name << ":" << getName() << "} "

#define JOBS_DEFAULT_PRIORITY 500

// Declare the class we're going to implement below
class BedrockPlugin_Jobs : public BedrockNode::Plugin
{
public:
    // Implement base class interface
    virtual string getName( ) { return "Jobs"; }
    virtual void   upgradeDatabase( BedrockNode* node, SQLite& db );
    virtual bool   peekCommand   ( BedrockNode* node, SQLite& db, BedrockNode::Command* command );
    virtual bool   processCommand( BedrockNode* node, SQLite& db, BedrockNode::Command* command );
    virtual void   test( BedrockTester* tester );

private:
    // Helper functions
    string _constructNextRunDATETIME( const string& lastScheduled, const string& lastRun, const string& repeat );
    bool   _validateRepeat( const string& repeat ) { return !_constructNextRunDATETIME( "", "", repeat ).empty(); }
};

// Register for auto-discovery at boot
BREGISTER_PLUGIN( BedrockPlugin_Jobs );


// ==========================================================================
void BedrockPlugin_Jobs::upgradeDatabase( BedrockNode* node, SQLite& db )
{
    // Create or verify the jobs table
    bool ignore;
    while( !db.verifyTable( "jobs",
        "CREATE TABLE jobs ( "
            "created  TIMESTAMP NOT NULL, "
            "jobID    INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
            "state    TEXT NOT NULL, "
            "name     TEXT NOT NULL, "
            "nextRun  TIMESTAMP NOT NULL, "
            "lastRun  TIMESTAMP, "
            "repeat   TEXT NOT NULL, "
            "data     TEXT NOT NULL, "
            "priority INTEGER NOT NULL DEFAULT " + SToStr(JOBS_DEFAULT_PRIORITY) + " ) ",

        ignore
    ) ) {
        // If it couldn't create or verify, something's wrong
        // **FIXME: Remove this after upgrade
        SASSERT( db.write( "ALTER TABLE jobs ADD COLUMN priority INTEGER NOT NULL DEFAULT " + SToStr(JOBS_DEFAULT_PRIORITY) + ";" ) );
    }

    // These indexes are not used by the Bedrock::Jobs plugin, but provided for easy analysis
    // using the Bedrock::DB plugin.
    SASSERT( db.write( "CREATE INDEX IF NOT EXISTS jobsState   ON jobs ( state   );" ) );
    SASSERT( db.write( "CREATE INDEX IF NOT EXISTS jobsName    ON jobs ( name    );" ) );
    SASSERT( db.write( "CREATE INDEX IF NOT EXISTS jobsNextRun ON jobs ( nextRun );" ) );
    SASSERT( db.write( "CREATE INDEX IF NOT EXISTS jobsLastRun ON jobs ( lastRun );" ) );

    // This index is used to optimize the Bedrock::Jobs::GetJob call.
    SASSERT( db.write( "CREATE INDEX IF NOT EXISTS jobsStateNextRunPriorityName ON jobs ( state, nextRun, priority, name );" ) );
}

// ==========================================================================
bool BedrockPlugin_Jobs::peekCommand( BedrockNode* node, SQLite& db, BedrockNode::Command* command ) 
{
    // Pull out some helpful variables
    SData&  request  = command->request;
    SData&  response = command->response;
    STable& content  = command->jsonContent;

    // ----------------------------------------------------------------------
    if( SIEquals( request.methodLine, "GetJob" ) )
    {
        ///p - GetJob( name )
        ///p 
        ///p     Atomically dequeues exactly one job, if available.
        ///p
        ///p     Parameters:
        ///p     - name - name pattern of jobs to match 
        ///p     - timeout - (optional) maximum time (in ms) to wait, default forever
        ///p
        ///p     Returns:
        ///p     - 200 - OK
        ///p         . jobID - unique ID of the job
        ///p         . name  - name of the actual job matched
        ///p         . data  - JSON data associated with this job
        ///p     - 303 - Timeout
        ///p     - 404 - No jobs found
        ///p 
        BVERIFY_ATTRIBUTE_SIZE( "name", 1, BMAX_SIZE_SMALL );

        // Get the list
        SQResult result;
        const string& name = request["name"];
        if( !db.read( "SELECT 1 "
                      "FROM jobs "
                      "WHERE state='QUEUED' "
                      "  AND " + SCURRENT_TIMESTAMP() + ">=nextRun " +
                      "  AND name GLOB " + SQ(name) + " " +
                      "ORDER BY priority DESC, nextRun ASC "
                      "LIMIT 1;", result ) ) {
            throw "502 Query failed";
        }

        // If we didn't get any results, just return an empty list
        if( result.empty() || SToInt( result[0][0] ) == 0 )
        {
            // Did the caller set "Connection: wait"?  If so, put a "hold"
            // on this request -- we'll clear the hold when we get a new
            // job.
            if( SIEquals( request["Connection"], "wait" ) )
            {
                // Place a hold on this request waiting for new jobs in this
                // state.
                SINFO( "No results found and 'Connection: wait'; placing request on hold until we get a new job matching name '" << request["name"] << "'" );
                request["HeldBy"] = "Jobs:" + name;
                response.clear( ); // Clear default response so we don't accidentally think we're done
                return false; // Not processed
            }
            else
            {
                // Don't hold, just respond with no results
                throw "404 No job found";
            }
        }

        // Looks like there might be results -- queue this for processing
        SINFO( "Found results, but waiting for processCommand to update" );
        return false;
    }

    // ----------------------------------------------------------------------
    else if( SIEquals( request.methodLine, "QueryJob" ) )
    {
        ///p - QueryJob( jobID )
        ///p 
        ///p     Returns all known information about a given job.
        ///p
        ///p     Parameters:
        ///p     - jobID - Identifier of the job to query
        ///p
        ///p     Returns:
        ///p     - 200 - OK
        ///p         . created - creation time of this job
        ///p         . jobID - unique ID of the job
        ///p         . state - One of QUEUED, RUNNING, FINISHED
        ///p         . name  - name of the actual job matched
        ///p         . nextRun - timestamp of next scheduled run
        ///p         . lastRun - timestamp it was last run
        ///p         . repeat - recurring description
        ///p         . data - JSON data associated with this job
        ///p     - 404 - No jobs found
        ///p 
        BVERIFY_ATTRIBUTE_INT64( "jobID", 1 );

        // Verify there is a job like this
        SQResult result;
        if( !db.read( "SELECT created, jobID, state, name, nextRun, lastRun, repeat, data "
                      "FROM jobs "
                      "WHERE jobID=" + SQ(request.calc64("jobID")) + ";",
                      result ) ) {
            throw "502 Select failed";
        }
        if( result.empty() ) {
            throw "404 No job with this jobID";
        }
        content["created"] = result[0][0];
        content["jobID"]   = result[0][1];
        content["state"]   = result[0][2];
        content["name"]    = result[0][3];
        content["nextRun"] = result[0][4];
        content["lastRun"] = result[0][5];
        content["repeat"]  = result[0][6];
        content["data"]    = result[0][7];
        return true; // Successfully processed
    }
    else if (SIEquals(request.methodLine, "CreateJob")) {
        // If unique flag was passed and the job exist in the DB, then we can finish the command without escalating to master.
        if (!request.test("unique")) {
            return false;
        }
        SQResult result;
        SINFO("Unique flag was passed, checking existing job with name " << request["name"]);
        if (!db.read("SELECT jobID "
                      "FROM jobs "
                      "WHERE name=" + SQ(request["name"]) + ";", result)) {
            throw "502 Select failed";
        }
        if (result.empty()) {
            return false;
        }

        SINFO("Job already existed and unique flag was passed, reusing existing job " << result[0][0]);
        content["jobID"] = result[0][0];
        return true;
    }

    // Didn't recognize this command
    return false;
}

// ==========================================================================
bool BedrockPlugin_Jobs::processCommand( BedrockNode* node, SQLite& db, BedrockNode::Command* command ) 
{
    // Pull out some helpful variables
    SData&  request  = command->request;
  //SData&  response = command->response; -- Not used
    STable& content  = command->jsonContent;

    // ----------------------------------------------------------------------
    if( SIEquals( request.methodLine, "CreateJob" ) )
    {
        ///p - CreateJob( name, [data], [firstRun], [repeat] )
        ///p 
        ///p     Creates a "job" for future processing by a worker. 
        ///p
        ///p     Parameters:
        ///p     - name  - An arbitrary string identifier (case insensitive)
        ///p     - data  - A JSON object describing work to be done (optional)
        ///p     - firstRun - A "YYYY-MM-DD HH:MM:SS" datetime of when
        ///p                  this job should next execute (optional)
        ///p     - repeat - A description of how to repeat (optional)
        ///p     - priority - High priorities go first (optional, default 500)
        ///p     - unique - if true, it will check that no other job with this name already exists, if it does it will return that jobID
        ///p
        ///p     Returns:
        ///p     - jobID - Unique identifier of this job
        ///p 
        BVERIFY_ATTRIBUTE_SIZE( "name", 1, BMAX_SIZE_SMALL );

        // If unique flag was passed and the job exist in the DB, then we can finish the command without escalating to master.
        if (request.test("unique")) {
            SQResult result;
            SINFO("Unique flag was passed, checking existing job with name " << request["name"]);
            if (!db.read("SELECT jobID "
                                 "FROM jobs "
                                 "WHERE name=" + SQ(request["name"]) + ";", result)) {
                throw "502 Select failed";
            }
            if (!result.empty()) {
                SINFO("Job already existed and unique flag was passed, reusing existing job " << result[0][0]);
                content["jobID"] = result[0][0];
                return true;
            }
        }

        // If no "firstRun" was provided, use right now
        const string& safeFirstRun =
            request["firstRun"].empty() ? 
                SCURRENT_TIMESTAMP() : SQ(request["firstRun"]);

        // If no data was provided, use an empty object
        const string& safeData =
            request["data"].empty() ? 
                SQ("{}") : SQ(request["data"]);

        // If a repeat is provided, validate it
        if( request.isSet("repeat") && !_validateRepeat( request["repeat"] ) )
            throw "402 Malformed repeat";

        // If no priority set, set it
        int priority = request.calc("priority") ? request.calc("priority") : JOBS_DEFAULT_PRIORITY;

        // Create this new job
        db.write( "INSERT INTO jobs ( created, state, name, nextRun, repeat, data, priority ) "
                  "VALUES( " +
                    SCURRENT_TIMESTAMP()              + ", " +
                    SQ( "QUEUED" )                    + ", " +
                    SQ( request["name"] )             + ", " +
                    safeFirstRun                      + ", " +
                    SQ( SToUpper(request["repeat"]) ) + ", " +
                    safeData                          + ", " +
                    SQ( priority )                    + " );" );

        // Release workers waiting on this state
        node->clearCommandHolds( "Jobs:" + request["name"] );

        // Return the new jobID
        const int64_t lastInsertRowID = db.getLastInsertRowID();
        const int64_t maxJobID = SToInt64(db.read("SELECT MAX(jobID) FROM jobs;"));
        if (lastInsertRowID != maxJobID) {
            SALERT("We might be returning the wrong jobID maxJobID=" << maxJobID << " lastInsertRowID=" << lastInsertRowID);
        }
        content["jobID"] = SToStr(lastInsertRowID);
        return true; // Successfully processed
    }

    // ----------------------------------------------------------------------
    else if( SIEquals( request.methodLine, "GetJob" ) )
    {
        // If we're here it's because peekCommand found some data;
        // re-execute the query for real now.
        SQResult result;
        const string& name = request["name"];
        if( !db.read( "SELECT jobID, name, data "
                      "FROM jobs "
                      "WHERE state='QUEUED' "
                      "  AND " + SCURRENT_TIMESTAMP() + ">=nextRun " +
                      "  AND name GLOB " + SQ(name) + " " +
                      "ORDER BY priority DESC, nextRun ASC "
                      "LIMIT 1;", result ) ) {
            throw "502 Query failed";
        }

        // Are there any results?
        if( result.empty() ) {
            // Ah, there were before, but aren't now -- nothing found
            // **FIXME: If "Connection: wait" should re-apply the hold.  However, this is super edge
            //          as we could only get here if the job somehow got consumed between the peek
            //          and process -- which could happen during heavy load.  But it'd just return
            //          no results (which is correct) faster than it would otherwise time out.  Either
            //          way the worker will likely just loop, so it doesn't really matter.
            throw "404 No job found";
        }
        SASSERT( result.size()==1 && result[0].size()==3 );

        // Update the state of that job
        if( !db.write( "UPDATE jobs "
                       "SET state='RUNNING', lastRun=" + SCURRENT_TIMESTAMP() +
                       " WHERE jobID=" + result[0][0] + ";" ) )
            throw "502 Update failed";

        // Construct the body
        content["jobID"] = result[0][0];
        content["name"]  = result[0][1];
        content["data"]  = result[0][2];
        return true; // Successfully processed
    }

    // ----------------------------------------------------------------------
    else if( SIEquals( request.methodLine, "UpdateJob" ) )
    {
        ///p - UpdateJob( jobID, data )
        ///p 
        ///p     Atomically updates the data associated with a job.
        ///p
        ///p     Parameters:
        ///p     - jobID - ID of the job to delete
        ///p     - data  - A JSON object describing work to be done
        ///p 
        BVERIFY_ATTRIBUTE_INT64( "jobID", 1 );
        BVERIFY_ATTRIBUTE_SIZE( "data", 1, BMAX_SIZE_BLOB );

        // Verify there is a job like this
        SQResult result;
        if( !db.read( "SELECT 1 "
                      "FROM jobs "
                      "WHERE jobID=" + SQ(request.calc64("jobID")) + ";",
                      result ) ) {
            throw "502 Select failed";
        }
        if( result.empty() || !SToInt(result[0][0]) ) {
            throw "404 No job with this jobID";
        }

        // Update the data
        if( !db.write( "UPDATE jobs "
                       "SET data=" + SQ(request["data"]) + " "
                       "WHERE jobID=" + SQ(request.calc64("jobID")) + ";" ) ) {
            throw "502 Update failed";
        }
        return true; // Successfully processed
    }

    // ----------------------------------------------------------------------
    else if( SIEquals( request.methodLine, "RetryJob"  ) ||
             SIEquals( request.methodLine, "FinishJob" ) )
    {
        ///p - RetryJob( jobID, delay, [data] )
        ///p
        ///p     Re-queues a RUNNING job for "delay" seconds in the future,
        ///p     unless the job is configured to "repeat" in which it will
        ///p     just schedule for the next repeat time.
        ///p     Use this when a job was only partially completed but
        ///p     interrupted in a non-fatal way.
        ///p
        ///p     Parameters:
        ///p     - jobID  - ID of the job to retry
        ///p     - delay  - Number of seconds to wait before retrying
        ///p     - data   - Data to associate with this finsihed job
        ///p 
        ///p - FinishJob( jobID, [data] )
        ///p
        ///p     Finishes a job.  If it's set to recur, reschedules it. If
        ///p     not recurring, deletes it.
        ///p
        ///p     Parameters:
        ///p     - jobID  - ID of the job to finish
        ///p     - data   - Data to associate with this finsihed job
        ///p 
        BVERIFY_ATTRIBUTE_INT64( "jobID", 1 );

        // Verify there is a job like this and it's running
        SQResult result;
        if( !db.read( "SELECT state, nextRun, lastRun, repeat "
                      "FROM jobs "
                      "WHERE jobID=" + SQ(request.calc64("jobID")) + ";",
                      result ) ) {
            throw "502 Select failed";
        }
        if( result.empty() ) {
            throw "404 No job with this jobID";
        }
        const string& state   = result[0][0];
        const string& nextRun = result[0][1];
        const string& lastRun = result[0][2];
              string  repeat  = result[0][3];

        // Make sure we're finishing a job that's actually running
        if( state != "RUNNING" ) {
            SWARN( "Trying to finish job#" << request["jobID"] << ", but isn't RUNNING (" << state << ")" );
            throw "405 Can only retry/finish RUNNING jobs";
        }

        // If we're doing RetryJob and there isn't a repeat, construct one with the delay
        if (repeat.empty() && SIEquals(request.methodLine, "RetryJob")) {
            // Make sure there is a delay
            int64_t delay = request.calc64("delay");
            if (delay<0) {
                throw "402 Must specify a non-negative delay when retrying";
            }
            repeat = "FINISHED, +" + SToStr(delay) + " SECONDS";
        }

        // Are we rescheduling?
        if( !repeat.empty() )
        {
            // Configured to repeat.  The "nextRun" at this point is still storing the last time this job was *scheduled* to
            // be run; lastRun contains when it was *actually* run.
            const string& lastScheduled = nextRun;
            const string& newNextRun = _constructNextRunDATETIME( lastScheduled, lastRun, repeat );
            if( newNextRun.empty() ) {
                throw "402 Malformed repeat";
            }
            SINFO( "Rescheduling job#" << request["jobID"] << ": " << newNextRun );
            list<string> updateList;
            updateList.push_back( "nextRun=" + newNextRun );
            updateList.push_back( "state='QUEUED'" );

            // Are we updating the data too? (This can be used to pass state from worker to worker
            // between runs of the same job.)
            if( request.isSet("data") ) {
                // Update the data too
                updateList.push_back( "data=" + SQ(request["data"]) );
            }

            // Update this job
            if( !db.write( "UPDATE jobs SET " +
                           SComposeList( updateList ) +
                          "WHERE jobID=" + SQ(request.calc64("jobID")) + ";" ) ) {
                throw "502 Update failed";
            }
        }
        else 
        {
            // Delete this job
            SASSERT(!SIEquals(request.methodLine, "RetryJob"));
            if( !db.write( "DELETE FROM jobs WHERE jobID=" + SQ(request.calc64("jobID")) + ";" ) ) {
                throw "502 Delete failed";
            }
        }

        // Successfully processed
        return true; 
    }

    // ----------------------------------------------------------------------

    else if( SIEquals( request.methodLine, "FailJob" ) )
    {
        ///p - FailJob( jobID, [data] )
        ///p
        ///p     Fails a job.
        ///p
        ///p     Parameters:
        ///p     - jobID - ID of the job to fail
        ///p     - data  - Data to associate with this failed job
        ///p
        BVERIFY_ATTRIBUTE_INT64( "jobID", 1 );

        // Verify there is a job like this and it's running
        SQResult result;
        if( !db.read( "SELECT state, nextRun, lastRun, repeat "
                     "FROM jobs "
                     "WHERE jobID=" + SQ(request.calc64("jobID")) + ";",
                     result ) ) {
            throw "502 Select failed";
        }
        if( result.empty() ) {
            throw "404 No job with this jobID";
        }
        const string& state = result[0][0];

        // Make sure we're failing a job that's actually running
        if( state != "RUNNING" ) {
            SWARN( "Trying to fail job#" << request["jobID"] << ", but isn't RUNNING (" << state << ")" );
            throw "405 Can only fail RUNNING jobs";
        }

        // Are we updating the data too?
        list<string> updateList;
        if( request.isSet("data") ) {
            // Update the data too
            updateList.push_back( "data=" + SQ(request["data"]) );
        }

        // Not repeating; just finish
        updateList.push_back( "state='FAILED'" );

        // Update this job
        if( !db.write( "UPDATE jobs SET " +
                      SComposeList( updateList ) +
                      "WHERE jobID=" + SQ(request.calc64("jobID")) + ";" ) ) {
            throw "502 Fail failed";
        }

        // Successfully processed
        return true;
    }

    // ----------------------------------------------------------------------

    else if( SIEquals( request.methodLine, "DeleteJob" ) )
    {
        ///p - DeleteJob( jobID )
        ///p 
        ///p     Deletes a given job.
        ///p
        ///p     Parameters:
        ///p     - jobID - ID of the job to delete
        ///p 
        BVERIFY_ATTRIBUTE_INT64( "jobID", 1 );

        // Verify there is a job like this and it's not running
        SQResult result;
        if( !db.read( "SELECT state "
                      "FROM jobs "
                      "WHERE jobID=" + SQ(request.calc64("jobID")) + ";",
                      result ) ) {
            throw "502 Select failed";
        }
        if( result.empty() ) {
            throw "404 No job with this jobID";
        }
        if( result[0][0]=="RUNNING" ) {
            throw "405 Can't delete a RUNNING job";
        }

        // Delete the job
        if( !db.write( "DELETE FROM jobs "
                      "WHERE jobID=" + SQ(request.calc64("jobID")) + ";" ) ) {
            throw "502 Delete failed";
        }

        // Successfully processed
        return true; 
    }

    // Didn't recognize this command
    return false;
}

// Under this line we don't know the "node", so remove from the log prefix
#undef SLOGPREFIX
#define SLOGPREFIX "{" << getName() << "} "

// ==========================================================================
void BedrockPlugin_Jobs::test( BedrockTester* tester )
{
STestTimer test( "Testing constructNextRunDATETIME" );
    // Confirm parsing
    STESTASSERT(  _validateRepeat( "SCHEDULED, +1 HOUR" ) );
    STESTASSERT(  _validateRepeat( "FINISHED, +7 DAYS, WEEKDAY 4" ) );
    STESTASSERT(  _validateRepeat( "STARTED, +1 MONTH, -1 DAY, START OF DAY, +4 HOURS" ) );
    STESTASSERT( !_validateRepeat( "FINISHED, WEEKDAY 7" ) ); // only 0-6 weekdays
}

// ==========================================================================
string BedrockPlugin_Jobs::_constructNextRunDATETIME( const string& lastScheduled, const string& lastRun, const string& repeat )
{
    // Some "canned" times for convenience
    if( SIEquals(repeat, "HOURLY") ) return "STRFTIME( '%Y-%m-%d %H:00:00', DATETIME( " + SCURRENT_TIMESTAMP() + ", '+1 HOUR' ) )";
    if( SIEquals(repeat, "DAILY" ) ) return "DATETIME( " + SCURRENT_TIMESTAMP() + ", '+1 DAY', 'START OF DAY'  )";
    if( SIEquals(repeat, "WEEKLY") ) return "DATETIME( " + SCURRENT_TIMESTAMP() + ", '+1 DAY', 'WEEKDAY 0', 'START OF DAY' )";

    // Not canned, split the advanced repeat into its parts
    list<string> parts = SParseList( SToUpper(repeat) );
    if( parts.size() < 2 ) {
        SWARN( "Syntax error, failed parsing repeat '" << repeat << "': too short." );
        return "";
    }

    // Make sure the first part indicates the base (eg, what we are modifying)
    list<string> safeParts;
    string base = parts.front();
    parts.pop_front();
    if( base == "SCHEDULED" ) {
        safeParts.push_back( SQ(lastScheduled) );
    }
    else if( base == "STARTED" ) {
        safeParts.push_back( SQ(lastRun) );
    }
    else if( base == "FINISHED" ) {
        safeParts.push_back( SCURRENT_TIMESTAMP() );
    }
    else {
        SWARN( "Syntax error, failed parsing repeat '" << repeat << "': missing base (" << base << ")" );
        return "";
    }

    // Validate the sqlite date modifiers
    // See: https://www.sqlite.org/lang_datefunc.html
    SFOREACH( list<string>, parts, partIt )
    {
        // Simple regexp validation
        const string& part = *partIt;
        if( SREMatch( "^(\\+|-)\\d{1,3} (YEAR|MONTH|DAY|HOUR|MINUTE|SECOND)S?$", part ) ) {
            safeParts.push_back( SQ(part) );
        }
        else if( SREMatch( "^START OF (DAY|MONTH|YEAR)$", part ) ) {
            safeParts.push_back( SQ(part) );
        }
        else if( SREMatch( "^WEEKDAY [0-6]$", part ) ) {
            safeParts.push_back( SQ(part) );
        }
        else {
            // Malformed part
            SWARN( "Syntax error, failed parsing repeat '" << repeat << "' on part '" << part << "'" );
            return "";
        }
    }

    // Combine the parts together and return the full DATETIME statement
    return "DATETIME( " + SComposeList(safeParts) + " )";
}
