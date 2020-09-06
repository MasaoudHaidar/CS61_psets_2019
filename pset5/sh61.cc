#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>



#include <map>
#include <unistd.h>


//Handling the ctrl-c
volatile sig_atomic_t flag=0;
void signal_handler(int signal){
    (void) signal;
    flag=1;
}

//Handling the zombie processes:
//map zomb keeps track of processes that we haven't waited for yet and
//void remov_zomb waits for all processes marked in the map zomb

std::map<pid_t,bool> zomb;

void remov_zomb(){
    for (auto it=zomb.begin();it!=zomb.end();it++){
        if (!it->second) continue; //This means they have already exited

        int status;
        pid_t finished=waitpid(it->first,&status,WNOHANG);
        //WNOHANG is used instead of 0 to avoid bringing a background
        //process to the front.
        if (finished==0) continue;
        it->second=0;//Marked the killed zombies.
    }
}

//Structs used to build the tree of commands:
//A command is a single process
//A pipeline is a group of commands
//A conditional_list is a group of pipelines connected together with either
//&& or ||
//A command_list is a group of conditional_list s connected together with ;
//or &
struct command{
    std::vector<std::string> args;
    //Pipelines:
    int my_read_end=-1;//am I on the right side of a pipe?
    //If yes, what should I be reading from

    //Redirections:
    std::string out_red;
    std::string in_red;
    std::string err_red;


    //The command's functions:
    //make_child is usually used to execute this command, unless it is cd
    //cd is executed using my_cd
    pid_t make_child(bool is_last, pid_t* gpid, bool is_fore);
    pid_t my_cd(bool is_last);
};


struct pipeline {
    std::vector<command> pipeline_list;
    pid_t pid;       //the pid of the last command in the pipeline
    int cond_type=0; //condition type before this pipeline: && or ||

    //The pipeline's function:
    //It executes make_child on all its child commands
    pid_t make_pipeline(bool is_fore);
};

struct conditional_list{
    std::vector<pipeline> child_pipelines;
    int com_type=0; //The type of the list: & or ;

    bool last_con=1;//What is the exit status of the last executed command
    //in this list.

    int last_pos=0; //The position of the last executed command in this list

};

struct command_list{
    std::vector<conditional_list> child_conditionals;

    std::vector< std::pair <int , int> > waiting_list;
    //The waiting_list is used to keep track of the current running childs
    //The first part of the pair points to the child conditional_list being
    //run and the second part of the pair points to the position in that list

};

// command EXECUTION
// my_cd: executes the cd commands
// returns the cd's pid on success if this is the last command in a pipeline
// and returns -1 otherwise

// make_child: execute the current command
// forks a new child and returns its pid if it is the last command in a pipeline,
// otherwise it returns file descriptor of the file that the next command in the
// pipeline should read from

// make_pipeline: runs all the commands in the pipeline
// returns the pid of the last command.


pid_t command::my_cd(bool is_last){
    const char* path=this->args[1].c_str();
    int status; //What should the return status of cd be?
    if (!chdir(path)){
        //successfully changed the directory
        status=0;
    }
    else {
        status=1;
        //We should print an error message or redirect the error message
        if (this->err_red.size()==0){
            //No redirection required
            fprintf(stderr,"cd: %s: No such file or directory",path);
        }
        else{
            int err_fd=openat(AT_FDCWD,this->err_red.c_str(),O_WRONLY|O_CREAT|O_TRUNC, 0666);
            if (err_fd<0){
                //We can't find the file where we should print the error message
                fprintf(stderr,"No such file or directory ");
            }
            else{
                //Print the error message to the required file
                std::string err_mes="cd: "+this->args[1]+": No such file or directory";
                //For example:       cd:  /nonexistingfile: No such file of directory
                int nwritten=write(err_fd,(void*)err_mes.c_str(),err_mes.size());
                assert(nwritten==(int)err_mes.size());
            }
        }
    }

    //After we have finished executing, we create a child that will exit with
    //the required exit statues. This step is not necessary for the cd command
    //itself to work, but it makes it very easy to make the command consistent with
    // the rest of the code.
    int new_pid=fork();
    if (new_pid==0) _exit(status);
    if (is_last)
        return new_pid;
    else return -1;
}

pid_t command::make_child(bool is_last,pid_t* gpid, bool is_fore){
    //If the command is cd we should use the cd function:
    const char* file_name = this->args[0].c_str();
    if (this->args[0]=="cd"){
            return my_cd(is_last);
    }
    //If this is not the last command in a pipeline we should create a new pipe:
    int pfd[2]={-1,-1};
    if (!is_last){
        int r=pipe(pfd);
        assert(r==0);
    }
    //Fork the child:
    int new_pid=fork();
    if (new_pid==0){
        //The Child's code:

        //Keeping track of group id s:
        if (*gpid==0){
            //This is the first command in a pipeline
            setpgid(new_pid,new_pid);
        }
        else{
            //Use the gpid of the first command in the pipeline
            setpgid(new_pid,*gpid);
        }
        if (this->my_read_end>=0){
            //Read from the previous pipe
            dup2(this->my_read_end,STDIN_FILENO);
            close(this->my_read_end);
        }
        if (pfd[0]>=0){
            //Write to the following pipe
            dup2(pfd[1],STDOUT_FILENO);
            close(pfd[1]);
            close(pfd[0]);
        }
        //Redirections:
        if (this->in_red.size()!=0){
            int in_fd=openat(AT_FDCWD,this->in_red.c_str(),O_RDONLY);
            if (in_fd<0){
                fprintf(stderr,"No such file or directory ");
                _exit(1);
            }
            dup2(in_fd,STDIN_FILENO);
            close(in_fd);
        }
        if (this->out_red.size()!=0){
            int out_fd=openat(AT_FDCWD,this->out_red.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0666);
            if (out_fd<0){
                fprintf(stderr,"No such file or directory ");
                _exit(1);
            }
            dup2(out_fd,STDOUT_FILENO);
            close(out_fd);
        }
        if (this->err_red.size()!=0){
            int err_fd=openat(AT_FDCWD,this->err_red.c_str(),O_WRONLY|O_CREAT|O_TRUNC, 0666);
            if (err_fd<0){
                fprintf(stderr,"No such file or directory ");
                _exit(1);
            }
            dup2(err_fd,STDERR_FILENO);
            close(err_fd);
        }
        //Prepare the arguments array:
        const char* new_args[this->args.size()+1];
        for (int i=0;i<(int)this->args.size();i++){
            new_args[i]=this->args[i].c_str();
        }
        new_args[this->args.size()]=nullptr;

        //If the user pressed ctrl-c and this is a foreground process then
        //it should exit.
        if (flag && is_fore) _exit(1);
        //Execute the command:
        execvp(file_name,(char* const*)new_args);
        //We should never get here.
        _exit(1);
    }
    //The parent's code:

    //If this is the first command in a pipeline then we should save its
    //pid so we can use it as the gpid of the pipeline:
    if (*gpid==0)*gpid=new_pid;
    if (is_fore) claim_foreground(*gpid);

    //Keep track of the pid so that you can kill it when it becomes a zombie
    zomb[new_pid]=1;

    //Clean up the files from the pipe that you aren't going to use.
    //If this is not the last command, we should keep track of the next read
    //end and return it.
    //If this is the last command in the pipeline, we should return the pid
    //of this command so we can use it later to wait for the pipeline.
    if (this->my_read_end>=0){
        close(this->my_read_end);
    }
    int next_read_end=-1;
    if (pfd[0]>=0){
        close(pfd[1]);
        next_read_end=pfd[0];
    }
    if (is_last) return new_pid;
    return next_read_end;
}


pid_t pipeline::make_pipeline(bool is_fore) {
    pid_t gpid=0;
    for (int i=0;i<(int)this->pipeline_list.size();i++){
        bool is_last=(i==(int)this->pipeline_list.size()-1);
        pid_t this_pid_or_next_read=this->pipeline_list[i].make_child(is_last,&gpid,is_fore);
        if (!is_last){
            this->pipeline_list[i+1].my_read_end=this_pid_or_next_read;
        }
        else {
            this->pid=this_pid_or_next_read;
            return this_pid_or_next_read;
        }
    }
    //Should never get here
    return -1;
}


// run(c)
//    Run the command list c.

void run(command_list* c) {
    if (c==nullptr) return;
    c->waiting_list.push_back({0,0});//Add the first conditional list
    int pos=1;//points to the next conditional list we should add.

    while (1){//This is not an infinite loop. There is a break.
        bool empty_list=1; //Have we finished everything on the waiting list?

        //Iterate through the waiting list and advance in the child conditional
        //lists whenever you can
        for (int i=0;i<(int)c->waiting_list.size();i++){
            std::pair <int,int> cur_pos=c->waiting_list[i];
            conditional_list* cur_list=&c->child_conditionals[cur_pos.first];
            pipeline* cur_com=&cur_list->child_pipelines[cur_pos.second];
            //We're at the pipeline cur_com in the conditional list cur_list
            if (!cur_list
            || cur_pos.second>=(int)cur_list->child_pipelines.size()
            || !cur_com) {
                //We have finished this conditional list
                if (cur_pos.second>=(int)cur_list->child_pipelines.size() &&
                cur_list->com_type==TYPE_SEQUENCE){
                    //If this was a sequence list and it has finished
                    //then we can mark it as a background list.
                    //This will allow as to add the next conditional list later
                    int status;
                    cur_com=&cur_list->child_pipelines[cur_list->last_pos];
                    pid_t finished=waitpid(cur_com->pid
                    , &status, WNOHANG);
                    if (finished==0){
                        continue;
                    }
                    zomb[cur_com->pid]=0;
                    cur_list->com_type=TYPE_BACKGROUND;
                }
                continue;
            }
            empty_list=0;//There still are commands to be executed so the waiting list isn't empty.
            if (cur_pos.second==0){
                //This is the first list in a conditional line, we should execute it.
                cur_com->make_pipeline((cur_list->com_type!=TYPE_SEQUENCE));//This statement tells make_child
                //whether this command is in the foreground, so that the ctrl-c could be handled.
                cur_list->last_pos=0;//The last position keeps track of the last
                //executed command in the conditional list.
                c->waiting_list[i].second++;
            }
            else if(cur_list->last_pos!=cur_pos.second){
                //In this case, this command should wait for the previous command to finish
                int status;
                pid_t finished=waitpid(cur_list->child_pipelines[cur_list->last_pos].pid
                , &status, WNOHANG);
                if (finished==0){
                    //If the previous command hasn't exited,
                    //we can't advance in this conditional list.
                    continue;
                }
                else{
                    //If the previous command has exited, we can mark that down:
                    //Remove it from the zombie list:
                    zomb[cur_list->child_pipelines[cur_list->last_pos].pid]=0;
                    //Save its exit status:
                    if (WEXITSTATUS(status) || WIFSIGNALED(status) != 0){
                        cur_list->last_con=0;
                    }
                    else {
                        cur_list->last_con=1;
                    }
                    //Mark that we have reached the current command so that we can
                    //execute it next time.
                    cur_list->last_pos=cur_pos.second;
                }
            }
            else{
                //We have reached this command, we should check if we should execute it:
                if ((cur_list->last_con==1 && cur_com->cond_type==TYPE_AND)||
                (cur_list->last_con==0 && cur_com->cond_type==TYPE_OR)){
                    //We should execute it and update our waiting list:
                    cur_com->make_pipeline((cur_list->com_type!=TYPE_SEQUENCE));
                    cur_list->last_pos=cur_pos.second;
                    c->waiting_list[i].second++;
                }
                else{
                    //We should go on.
                    c->waiting_list[i].second++;
                }
            }
        } //The end of the loop on the waiting list

        //Was this waiting list empty? (we reached the end of all its
        //child conditional lists)
        if (empty_list){
            if (c->child_conditionals[pos-1].com_type==TYPE_BACKGROUND){
                //We can go on to the next condtional list.
                c->waiting_list.clear();
            }
            else{
                //If the last conditional list was of type sequence, we should
                //waitpid for it
                std::pair <int,int> cur_pos=c->waiting_list[c->waiting_list.size()-1];
                conditional_list* cur_list=&c->child_conditionals[cur_pos.first];
                pipeline* cur_com=&cur_list->child_pipelines[cur_list->last_pos];
                int status;
                pid_t finished=waitpid(cur_com->pid,&status,0);
                (void) finished;
                claim_foreground(0);
                zomb[cur_com->pid]=0;
                c->waiting_list.clear();
            }
        }
        //If the waiting list is empty and we reached the end of the child
        //conditional lists, then we finished running the command list.
        if (empty_list && pos>=(int)c->child_conditionals.size()) break;

        //If we haven't reached the end of our child conditional lists, and the waiting list
        //is empty or in the background, then we should go on to the next conditional list.
        if ((c->child_conditionals[pos-1].com_type==TYPE_BACKGROUND || empty_list)
        && pos<(int)c->child_conditionals.size()){
            c->waiting_list.push_back({pos,0});
            pos++;
        }
    }
}


// parse_line(s)
//    Parse the command line in `s` and returns it as a command list.
// Returns `nullptr` if `s` is empty (only spaces).

command_list* parse_line(const char* s) {
    int type;
    std::string token;

    //Biggest to smallest:
    command_list* c=nullptr;
    conditional_list d;
    pipeline e;
    command f;
    //using a pointer and dynamic memory only for the command list makes
    //it easy to pass the memory leak tests without makeing destructor
    //functions.

    int next_cond_type=0;

    while ((s = parse_shell_token(s, &type, &token)) != nullptr) {
        if (!c) {c = new command_list;}

        if (type==TYPE_BACKGROUND || type==TYPE_SEQUENCE){
            //marks a new conditional list:

            e.pipeline_list.push_back(f);
            f.args.clear();
            f.err_red.clear();
            f.in_red.clear();
            f.out_red.clear();

            e.cond_type=next_cond_type;
            next_cond_type=0;
            d.child_pipelines.push_back(e);
            e.pipeline_list.clear();

            d.com_type=type;
            c->child_conditionals.push_back(d);
            d.child_pipelines.clear();
        }
        else if (type==TYPE_AND || type==TYPE_OR){
            //marks a new pipeline:
            e.pipeline_list.push_back(f);
            f.args.clear();
            f.err_red.clear();
            f.in_red.clear();
            f.out_red.clear();

            e.cond_type=next_cond_type;
            next_cond_type=type;
            d.child_pipelines.push_back(e);
            e.pipeline_list.clear();

        }
        else if (type==TYPE_PIPE){
            //marks a new command in the pipeline:
            e.pipeline_list.push_back(f);
            f.args.clear();
            f.err_red.clear();
            f.in_red.clear();
            f.out_red.clear();
        }
        else if (type==TYPE_REDIRECTION){
            //This is a redirection for the last command entered,
            std::string operat=token;
            s = parse_shell_token(s, &type, &token);
            if (operat=="<"){
                f.in_red=token;
            }
            else if (operat==">"){
                f.out_red=token;
            }
            else if (operat=="2>"){
                f.err_red=token;
            }
        }
        else {
            //This is an argument for a command.
            f.args.push_back(token);
        }
    }
    //After we finish the line, we should save the last commands entered:
    if (f.args.size()!=0){
        e.pipeline_list.push_back(f);
    }
    if (e.pipeline_list.size()!=0){
        e.cond_type=next_cond_type;
        d.child_pipelines.push_back(e);
    }
    if (d.child_pipelines.size()!=0){
        if (!c) {c = new command_list;}
        c->child_conditionals.push_back(d);
    }
    return c;
}


int main(int argc, char* argv[]) {
    FILE* pipeline_file = stdin;
    bool quiet = false;


    // Check for '-q' option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read pipelines from file
    if (argc > 1) {
        pipeline_file = fopen(argv[1], "rb");
        if (!pipeline_file) {
            perror(argv[1]);
            exit(1);
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(pipeline_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, pipeline_file) == nullptr) {
            if (ferror(pipeline_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(pipeline_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(pipeline_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete pipeline line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (command_list* c = parse_line(buf)) {
                set_signal_handler(SIGINT,signal_handler);//for step 8
                run(c);
                flag=0;//for step 8
                delete c;
            }
            bufpos = 0;
            needprompt = 1;
        }

        // Handle zombie processes:
        remov_zomb();
    }

    return 0;
}
