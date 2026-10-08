#include <array>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

// flag and function to handle ctrl+c
volatile sig_atomic_t ctrl_c_pressed = 0;
void handle_sigint(int sig) { ctrl_c_pressed = 1; }

// everything we need for a single command
struct Command {
	std::vector<std::string> args;
	std::string in_file;
	std::string out_file;
	bool wrt_override = false;
};

// everything we need to store pipe chains
struct Pipeline {
	std::vector<Command> commands = std::vector<Command>(1);
	std::string raw_input;
};

// everything we need to save and print history
struct History {
	// maximum amount of commands to store
	static constexpr size_t MAX_HIST = 100;
	std::array<Pipeline, MAX_HIST> history{};
	size_t hist_idx = 0; // next slot to add an entry
	size_t cur_size = 0;

	// adds command to history
	void add(const Pipeline &cmds) {
		history[hist_idx] = cmds;
		hist_idx = (hist_idx + 1) % MAX_HIST;
		if (cur_size < MAX_HIST) {
			++cur_size;
		}
	}

	// prints history with numbers in descending order, most recent at bottom.
	void print(std::ostream &out = std::cout) const {
		size_t start_idx = (hist_idx + MAX_HIST - cur_size) % MAX_HIST;
		for (size_t i = 0; i < cur_size; ++i) {
			out << "[ " << cur_size - i << "] "
				<< history[(start_idx + i) % MAX_HIST].raw_input << '\n';
		}
	}

	// retrieve command from history in order to use it again
	const Pipeline *get_cmd_redo(size_t cmd_num) const {
		if (cmd_num < 1 || cmd_num > cur_size)
			return nullptr;

		size_t idx = (hist_idx + MAX_HIST - cmd_num) % MAX_HIST;
		return &history[idx];
	}
};

// history for all functions to have access to, but hidden to outside files
// because in anonymous namespace.
History hist;

// --- functions ---

bool start_handler() {
	// ============ Beginning of code from stackoverflow. ============
	// Configure the signal handler for ctrl+c
	// Configure sigaction to prevent automatic system call restarts
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_sigint;
	sigemptyset(&sa.sa_mask);

	sa.sa_flags = 0;

	return sigaction(SIGINT, &sa, nullptr) == 0;
	// ============ End of code from stackoverflow. ============
}

// checks if a given command is referencing something in history
bool is_history_ref(const std::string &str) {
	if (str.size() < 2 || str[0] != '!')
		return false;

	for (size_t i = 1; i < str.size(); ++i) {
		if (!std::isdigit(static_cast<unsigned char>(str[i])))
			return false;
	}

	return true;
}

// checks if given command is to print out history
bool is_history_cmd(const Pipeline &cmds) {
	return cmds.commands.size() == 1 && cmds.commands[0].args[0] == "history";
}

// changes directory, returns true if successful and false if not
bool change_dir(const Command &cmd) {
	if (cmd.args.size() > 2) {
		std::cerr << "Too many arguments\n";
		return false;
	}
	if (cmd.args.size() == 1) {
		std::cerr << "Must specify path\n";
		return false;
	}

	// check if chdir failed
	if (chdir(cmd.args[1].data()) == -1) {
		if (errno == ENOENT)
			std::cerr << "Could not find specified path " << cmd.args[1]
					  << '\n';
		else if (errno == ENOTDIR)
			std::cerr << "Specified path " << cmd.args[1]
					  << " is not a directory" << '\n';
		else
			std::cerr << "Command cd failed\n";
		return false;
	}
	return true;
}

// converts strings to char* to use with execvp
std::vector<char *> to_argv(Command &cmd) {
	std::vector<char *> argv;
	for (auto &arg : cmd.args)
		argv.push_back(arg.data());
	argv.push_back(nullptr);
	return argv;
}

// opens redirected files if necessary, changing stdout and stdin to point to
// them, then executes given command
void execute(Command &cmd) {
	std::vector<char *> argv = to_argv(cmd);

	if (!cmd.in_file.empty()) {
		// save file descriptor given by open and check if open failed
		int in_fd = open(cmd.in_file.data(), O_RDONLY);
		if (in_fd == -1) {
			std::cerr << "Failed to open file " << cmd.in_file << '\n';
			_exit(1);
		}
		// change stdin to point to file
		dup2(in_fd, STDIN_FILENO);
		close(in_fd);
	}
	if (!cmd.out_file.empty()) {
		int flags = O_WRONLY | O_CREAT;
		if (cmd.wrt_override)
			flags |= O_TRUNC;
		else
			flags |= O_EXCL;

		// save file descriptor given by open and check if open failed
		int out_fd = open(cmd.out_file.data(), flags, 0644);
		if (out_fd == -1) {
			if (errno == EEXIST)
				std::cerr << "Failed to open file " << cmd.out_file
						  << ". Use >! to overwrite existing file.\n";
			else
				std::cerr << "Failed to open file " << cmd.out_file << '\n';
			_exit(1);
		}
		// change stdout to point to file
		dup2(out_fd, STDOUT_FILENO);
		close(out_fd);
	}
	// print history here so it works with pipes and redirects
	if (cmd.args[0] == "history") {
		hist.print();
		std::cout.flush();
		_exit(0);
	}
	execvp(argv[0], argv.data());
	std::cerr << argv[0] << ": command not found." << '\n';
}

// create child processes as necessary, looping through all
// piped commands.
void evaluate(Pipeline &cmds) {
	// run cd in the shell itself so we aren't just changing the directory of
	// the child
	if (cmds.commands.size() == 1 && (cmds.commands[0].args[0] == "cd")) {
		if (change_dir(cmds.commands[0]))
			hist.add(cmds);
		return;
	}

	// data from previous pipe
	int prev_fd = -1;

	// save pid of last child created in order to check if pipe chain was
	// successful
	pid_t last_pid = -1;

	for (size_t i = 0; i < cmds.commands.size(); ++i) {
		// pipe file descriptor array
		int pipe_fd[2];
		// check if there is a next command so that we know to send data through
		// pipe or to stdout
		bool has_next = i + 1 < cmds.commands.size();

		// create pipe if necessary
		if (has_next && pipe(pipe_fd) == -1) {
			perror("pipe failed");
			if (prev_fd != -1)
				close(prev_fd);
			break;
		}

		pid_t pid = fork();

		// fork failed. close whatever necessary and stop
		if (pid == -1) {
			if (prev_fd != -1)
				close(prev_fd);
			if (has_next) {
				close(pipe_fd[0]);
				close(pipe_fd[1]);
			}
			break;
		}

		// child: read from previous pipe if necessary, send stdout to pipe if
		// necessary.
		if (pid == 0) {
			if (prev_fd != -1) {
				dup2(prev_fd, STDIN_FILENO);
				close(prev_fd);
			}
			if (has_next) {
				dup2(pipe_fd[1], STDOUT_FILENO);
				close(pipe_fd[0]);
				close(pipe_fd[1]);
			}

			execute(cmds.commands[i]);
			_exit(127);
		}
		if (prev_fd != -1)
			close(prev_fd);
		if (has_next) {
			close(pipe_fd[1]);
			prev_fd = pipe_fd[0];
		}

		last_pid = pid;
	}

	pid_t done_pid;
	bool cmd_success = true;
	int status = 0;

	// ensure all processes end before returning so there aren't any zombies,
	// even when ctrl+c is used to interrupt current command.
	while ((done_pid = wait(&status)) > 0 ||
		   (done_pid == -1 && errno == EINTR)) {
		if (done_pid == last_pid)
			cmd_success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}

	// check that the command wasn't history so we dont save history to the
	// history (!# command gets very confusing if history is saved because
	// numbers are incremented after history is run)
	if (cmd_success && !is_history_cmd(cmds))
		hist.add(cmds);
}

// take input and tokenize, checking for pipes and stdin/stdout redirection
void parse_input() {
	std::string input;

	while (true) {
		// Prompt user and collect their input
		std::cout << "> ";

		// get input and check if ctrl+c was pressed
		if (!std::getline(std::cin, input)) {
			if (ctrl_c_pressed == 1) {
				std::cout << '\n';
				ctrl_c_pressed = 0;
				std::cin.clear();
				// create clean prompt
				continue;
			} else {
				break;
			}
		}

		Pipeline cmds;
		cmds.raw_input = input;

		std::istringstream stream(input);
		std::string token;
		bool first_token = true;
		bool after_redirect = false;
		bool stream_has_error = false;

		while (stream >> token) {
			if (first_token && token == "quit")
				return;

			first_token = false;

			// if pipe encountered, start new command
			if (token == "|") {
				cmds.commands.emplace_back();
				after_redirect = false;
			}
			// if stdin/stdout redirection encountered, save filenames
			else if (token == "<" || token == ">" || token == ">!") {
				std::string file;
				// make sure its actually a filename
				if (!(stream >> file) || file == "|" || file == "<" ||
					file == ">" || file == ">!") {
					std::cerr << "Expected filename after " << token << '\n';
					stream_has_error = true;
					break;
				}
				if (token == "<") {
					cmds.commands.back().in_file = file;
				} else {
					cmds.commands.back().out_file = file;
					if (token == ">!")
						cmds.commands.back().wrt_override = true;
				}
				after_redirect = true;
			} else if (!after_redirect) {
				cmds.commands.back().args.push_back(token);
			}
		}

		// ignore current command and create clean prompt if something went
		// wrong
		if (stream_has_error)
			continue;

		// if blank line create clean prompt
		if (cmds.commands.size() == 1 && cmds.commands[0].args.empty())
			continue;

		// check that no commands in pipe chain are empty
		bool has_empty = false;
		for (const auto &cmd : cmds.commands) {
			if (cmd.args.empty())
				has_empty = true;
		}
		if (has_empty) {
			std::cerr << "Missing command after '|'" << '\n';
			continue;
		}

		// check if we are referencing a command in history
		if (cmds.commands.size() == 1 &&
			is_history_ref(cmds.commands[0].args[0])) {
			size_t cmd_num;
			try {
				// turn the numeric part of the command into a size_t
				cmd_num = std::stoul(cmds.commands[0].args[0].substr(1));
			} catch (const std::out_of_range &) {
				std::cerr << cmds.raw_input << ": event not found\n";
				continue;
			}
			// find command we are redoing
			const Pipeline *cmd_to_redo = hist.get_cmd_redo(cmd_num);
			if (!cmd_to_redo) {
				std::cerr << cmds.raw_input << ": event not found\n";
				continue;
			}
			// overwrite with command we are redoing and echo it so the user
			// knows for sure what they ran
			cmds = *cmd_to_redo;
			std::cout << cmds.raw_input << '\n';
		}

		evaluate(cmds);
	}
}
} // namespace

int main() {
	if (!start_handler()) {
		std::cerr << "Failed to start signal handler." << std::endl;
	}
	parse_input();
	return 0;
}
