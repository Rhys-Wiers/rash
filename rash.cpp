#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t ctrl_c_pressed = 0;

void handle_sigint(int sig) { ctrl_c_pressed = 1; }

struct Command {
	std::vector<std::string> args;
	std::string in_file;
	std::string out_file;
	bool wrt_override = false;
};

using Pipeline = std::vector<Command>;

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

std::vector<char *> to_argv(Command &cmd) {
	std::vector<char *> argv;
	for (auto &s : cmd.args)
		argv.push_back(s.data());
	argv.push_back(nullptr);
	return argv;
}

void execute(Command &cmd) {

	std::vector<char *> argv = to_argv(cmd);

	if (!cmd.in_file.empty()) {
		int infile = open(cmd.in_file.data(), O_RDONLY);
		if (infile == -1) {
			std::cerr << "Failed to open file " << cmd.in_file << '\n';
			_exit(1);
		}
		dup2(infile, STDIN_FILENO);
		close(infile);
	}
	if (!cmd.out_file.empty()) {
		int flags = O_WRONLY | O_CREAT;
		if (cmd.wrt_override)
			flags |= O_TRUNC;
		else
			flags |= O_EXCL;

		int outfile = open(cmd.out_file.data(), flags, 0644);
		if (outfile == -1) {
			if (errno == EEXIST)
				std::cerr << "Failed to open file " << cmd.out_file
						  << ". Use >! to overwrite existing file." << '\n';
			else
				std::cerr << "Failed to open file " << cmd.out_file << '\n';
			_exit(1);
		}
		dup2(outfile, STDOUT_FILENO);
		close(outfile);
	}

	execvp(argv[0], argv.data());
	std::cerr << argv[0] << ": command not found." << '\n';
}

void evaluate(Pipeline &args) {
	// data from previous pipe
	int prev = -1;

	for (size_t i = 0; i < args.size(); ++i) {
		int fd[2];
		bool has_next = i + 1 < args.size();

		if (has_next && pipe(fd) == -1) {
			perror("pipe failed");
			if (prev != -1)
				close(prev);
			break;
		}

		pid_t pid = fork();

		if (pid == 0) {
			if (prev != -1) {
				dup2(prev, STDIN_FILENO);
				close(prev);
			}
			if (has_next) {
				dup2(fd[1], STDOUT_FILENO);
				close(fd[0]);
				close(fd[1]);
			}

			execute(args[i]);

			_exit(127);
		}
		if (prev != -1)
			close(prev);
		if (has_next) {
			close(fd[1]);
			prev = fd[0];
		}
		// parent
	}
	while (wait(nullptr) > 0) {
	}
}

void parse_input() {
	std::string input;

	while (true) {
		// Prompt user and collect their input
		std::cout << "> ";

		if (!std::getline(std::cin, input)) {
			if (ctrl_c_pressed == 1) {
				std::cout << '\n';
				ctrl_c_pressed = 0;
				std::cin.clear();
				continue;
			} else {
				break;
			}
		}

		bool error = false;
		Pipeline cmds(1);
		std::istringstream stream(input);
		std::string token;
		bool first = true;
		bool after_redirect = false;

		while (stream >> token) {
			if (first && token == "quit")
				return;

			first = false;

			if (token == "|") {
				cmds.emplace_back();
				after_redirect = false;
			} else if (token == "<" || token == ">" || token == ">!") {
				std::string file;
				if (!(stream >> file) || file == "|" || file == "<" ||
					file == ">" || file == ">!") {
					std::cerr << "Expected filename after " << token << '\n';
					error = true;
					break;
				}
				if (token == "<") {
					cmds.back().in_file = file;
				} else {
					cmds.back().out_file = file;
					if (token == ">!")
						cmds.back().wrt_override = true;
				}
				after_redirect = true;
			} else if (!after_redirect) {
				cmds.back().args.push_back(token);
			}
		}

		if (error)
			continue;

		if (cmds.size() == 1 && cmds[0].args.empty())
			continue;

		bool has_empty = false;
		for (const auto &arg : cmds) {
			if (arg.args.empty())
				has_empty = true;
		}
		if (has_empty) {
			std::cerr << "Missing command after '|'" << '\n';
			continue;
		}

		// Call function to fork and exec.
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
