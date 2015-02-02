/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "BionicDeathTest.h" // For selftest.

namespace testing {
namespace internal {

// Reuse of testing::internal::ColoredPrintf in gtest.
enum GTestColor {
  COLOR_DEFAULT,
  COLOR_RED,
  COLOR_GREEN,
  COLOR_YELLOW
};

void ColoredPrintf(GTestColor color, const char* fmt, ...);

}  // namespace internal
}  // namespace testing

using testing::internal::GTestColor;
using testing::internal::COLOR_DEFAULT;
using testing::internal::COLOR_RED;
using testing::internal::COLOR_GREEN;
using testing::internal::COLOR_YELLOW;
using testing::internal::ColoredPrintf;

constexpr int DEFAULT_GLOBAL_TEST_RUN_DEADLINE_MS = 60000;
constexpr int DEFAULT_GLOBAL_TEST_RUN_WARNLINE_MS = 2000;

// The time each test can run before killed for the reason of timeout.
// It takes effect only with --isolate option.
static int global_test_run_deadline_ms = DEFAULT_GLOBAL_TEST_RUN_DEADLINE_MS;

// The time each test can run before be warned for too much running time.
// It takes effect only with --isolate option.
static int global_test_run_warnline_ms = DEFAULT_GLOBAL_TEST_RUN_WARNLINE_MS;

// Return deadline duration for a test, in ms.
static int GetDeadlineInfo(const std::string& /*test_name*/) {
  return global_test_run_deadline_ms;
}

// Return warnline duration for a test, in ms.
static int GetWarnlineInfo(const std::string& /*test_name*/) {
  return global_test_run_warnline_ms;
}

static void PrintHelpInfo() {
  printf("Bionic Unit Test Options:\n"
         "  -j [JOB_COUNT] or -j[JOB_COUNT]\n"
         "      Run up to JOB_COUNT tests in parallel.\n"
         "      Use isolation mode, Run each test in a separate process.\n"
         "      If JOB_COUNT is not given, it is set to the count of available processors.\n"
         "  --no-isolate\n"
         "      Don't use isolation mode, run all tests in a single process.\n"
         "  --deadline=[TIME_IN_MS]\n"
         "      Run each test in no longer than [TIME_IN_MS] time.\n"
         "      It takes effect only in isolation mode. Deafult deadline is 60000 ms.\n"
         "  --warnline=[TIME_IN_MS]\n"
         "      Test running longer than [TIME_IN_MS] will be warned.\n"
         "      It takes effect only in isolation mode. Default warnline is 2000 ms.\n"
         "  --gtest-filter=POSITIVE_PATTERNS[-NEGATIVE_PATTERNS]\n"
         "      Used as a synonym for --gtest_filter option in gtest.\n"
         "\nDefault bionic unit test option is -j.\n"
         "\n");
}

enum TestResult {
  TEST_SUCCESS = 0,
  TEST_FAILED,
  TEST_TIMEOUT
};

class Test {
 public:
  Test() {} // For std::vector<Test>.
  explicit Test(const char* name) : name_(name) {}

  const std::string& GetName() const { return name_; }

  void SetResult(TestResult result) { result_ = result; }

  TestResult GetResult() const { return result_; }

  void SetTestTime(int64_t elapsed_time_ns) { elapsed_time_ns_ = elapsed_time_ns; }

  int64_t GetTestTime() const { return elapsed_time_ns_; }

  void AppendFailureMessage(const std::string& s) { failure_message_ += s; }

  const std::string& GetFailureMessage() const { return failure_message_; }

 private:
  const std::string name_;
  TestResult result_;
  int64_t elapsed_time_ns_;
  std::string failure_message_;
};

class TestCase {
 public:
  TestCase() {} // For std::vector<TestCase>.
  explicit TestCase(const char* name) : name_(name) {}

  const std::string& GetName() const { return name_; }

  void AppendTest(const char* test_name) {
    test_list_.push_back(Test(test_name));
  }

  size_t TestCount() const { return test_list_.size(); }

  std::string GetTestName(size_t test_id) const {
    VerifyTestId(test_id);
    return name_ + "." + test_list_[test_id].GetName();
  }

  Test& GetTest(size_t test_id) {
    VerifyTestId(test_id);
    return test_list_[test_id];
  }

  const Test& GetTest(size_t test_id) const {
    VerifyTestId(test_id);
    return test_list_[test_id];
  }

  void SetTestResult(size_t test_id, TestResult result) {
    VerifyTestId(test_id);
    test_list_[test_id].SetResult(result);
  }

  TestResult GetTestResult(size_t test_id) const {
    VerifyTestId(test_id);
    return test_list_[test_id].GetResult();
  }

  void SetTestTime(size_t test_id, int64_t elapsed_time_ns) {
    VerifyTestId(test_id);
    test_list_[test_id].SetTestTime(elapsed_time_ns);
  }

  int64_t GetTestTime(size_t test_id) const {
    VerifyTestId(test_id);
    return test_list_[test_id].GetTestTime();
  }

 private:
  void VerifyTestId(size_t test_id) const {
    if(test_id >= test_list_.size()) {
      fprintf(stderr, "test_id %zu out of range [0, %zu)\n", test_id, test_list_.size());
      exit(1);
    }
  }

 private:
  const std::string name_;
  std::vector<Test> test_list_;
};

// This is the file descriptor used by the child process to write failure message.
// The parent process will collect the information and dump to stdout / xml file.
static int child_output_fd;

class TestResultPrinter : public testing::EmptyTestEventListener {
 public:
  TestResultPrinter() : pinfo_(NULL) {}
  virtual void OnTestStart(const testing::TestInfo& test_info) {
    pinfo_ = &test_info; // Record test_info for use in OnTestPartResult.
  }
  virtual void OnTestPartResult(const testing::TestPartResult& result);

 private:
  const testing::TestInfo* pinfo_;
};

// Called after an assertion failure.
void TestResultPrinter::OnTestPartResult(const testing::TestPartResult& result) {
  // If the test part succeeded, we don't need to do anything.
  if (result.type() == testing::TestPartResult::kSuccess)
    return;

  // Print failure message from the assertion (e.g. expected this and got that).
  char buf[1024];
  snprintf(buf, sizeof(buf), "%s:(%d) Failure in test %s.%s\n%s\n", result.file_name(),
                                                                    result.line_number(),
                                                                    pinfo_->test_case_name(),
                                                                    pinfo_->name(),
                                                                    result.message());

  int towrite = strlen(buf);
  char* p = buf;
  while (towrite > 0) {
    ssize_t write_count = TEMP_FAILURE_RETRY(write(child_output_fd, p, towrite));
    if (write_count == -1) {
      fprintf(stderr, "failed to write child_output_fd: %s\n", strerror(errno));
      exit(1);
    } else {
      towrite -= write_count;
      p += write_count;
    }
  }
}

static int64_t NanoTime() {
  struct timespec t;
  t.tv_sec = t.tv_nsec = 0;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return static_cast<int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

static bool EnumerateTests(int argc, char** argv, std::vector<TestCase>& testcase_list) {
  std::string command;
  for (int i = 0; i < argc; ++i) {
    command += argv[i];
    command += " ";
  }
  command += "--gtest_list_tests";
  FILE* fp = popen(command.c_str(), "r");
  if (fp == NULL) {
    perror("popen");
    return false;
  }

  char buf[200];
  while (fgets(buf, sizeof(buf), fp) != NULL) {
    char* p = buf;

    while (*p != '\0' && isspace(*p)) {
      ++p;
    }
    if (*p == '\0') continue;
    char* start = p;
    while (*p != '\0' && !isspace(*p)) {
      ++p;
    }
    char* end = p;
    while (*p != '\0' && isspace(*p)) {
      ++p;
    }
    if (*p != '\0') {
      // This is not we want, gtest must meet with some error when parsing the arguments.
      fprintf(stderr, "argument error, check with --help\n");
      return false;
    }
    *end = '\0';
    if (*(end - 1) == '.') {
      *(end - 1) = '\0';
      testcase_list.push_back(TestCase(start));
    } else {
      testcase_list.back().AppendTest(start);
    }
  }
  int result = pclose(fp);
  return (result != -1 && WEXITSTATUS(result) == 0);
}

// Part of the following *Print functions are copied from external/gtest/src/gtest.cc:
// PrettyUnitTestResultPrinter. The reason for copy is that PrettyUnitTestResultPrinter
// is defined and used in gtest.cc, which is hard to reuse.
static void OnTestIterationStartPrint(const std::vector<TestCase>& testcase_list, size_t iteration,
                                      size_t iteration_count) {
  if (iteration_count > 1) {
    printf("\nRepeating all tests (iteration %zu) . . .\n\n", iteration);
  }
  ColoredPrintf(COLOR_GREEN,  "[==========] ");

  size_t testcase_count = testcase_list.size();
  size_t test_count = 0;
  for (const auto& testcase : testcase_list) {
    test_count += testcase.TestCount();
  }

  printf("Running %zu %s from %zu %s.\n",
         test_count, (test_count == 1) ? "test" : "tests",
         testcase_count, (testcase_count == 1) ? "test case" : "test cases");
  fflush(stdout);
}

static void OnTestEndPrint(const TestCase& testcase, size_t test_id) {
  TestResult result = testcase.GetTestResult(test_id);
  if (result == TEST_SUCCESS) {
    ColoredPrintf(COLOR_GREEN, "[    OK    ] ");
  } else if (result == TEST_FAILED) {
    ColoredPrintf(COLOR_RED, "[  FAILED  ] ");
  } else if (result == TEST_TIMEOUT) {
    ColoredPrintf(COLOR_RED, "[ TIMEOUT  ] ");
  }

  printf("%s", testcase.GetTestName(test_id).c_str());
  if (testing::GTEST_FLAG(print_time)) {
    printf(" (%" PRId64 " ms)\n", testcase.GetTestTime(test_id) / 1000000);
  } else {
    printf("\n");
  }

  const std::string& failure_message = testcase.GetTest(test_id).GetFailureMessage();
  printf("%s", failure_message.c_str());
  fflush(stdout);
}

static void OnTestIterationEndPrint(const std::vector<TestCase>& testcase_list, size_t /*iteration*/,
                                    int64_t elapsed_time_ns) {

  std::vector<std::string> fail_test_name_list;
  std::vector<std::pair<std::string, int64_t>> timeout_test_list;

  // For tests run exceed warnline but not timeout.
  std::vector<std::tuple<std::string, int64_t, int>> slow_test_list;
  size_t testcase_count = testcase_list.size();
  size_t test_count = 0;
  size_t success_test_count = 0;

  for (const auto& testcase : testcase_list) {
    test_count += testcase.TestCount();
    for (size_t i = 0; i < testcase.TestCount(); ++i) {
      TestResult result = testcase.GetTestResult(i);
      if (result == TEST_SUCCESS) {
        ++success_test_count;
      } else if (result == TEST_FAILED) {
        fail_test_name_list.push_back(testcase.GetTestName(i));
      } else if (result == TEST_TIMEOUT) {
        timeout_test_list.push_back(std::make_pair(testcase.GetTestName(i),
                                                   testcase.GetTestTime(i)));
      }
      if (result != TEST_TIMEOUT &&
          testcase.GetTestTime(i) / 1000000 >= GetWarnlineInfo(testcase.GetTestName(i))) {
        slow_test_list.push_back(std::make_tuple(testcase.GetTestName(i),
                                                 testcase.GetTestTime(i),
                                                 GetWarnlineInfo(testcase.GetTestName(i))));
      }
    }
  }

  ColoredPrintf(COLOR_GREEN,  "[==========] ");
  printf("%zu %s from %zu %s ran.", test_count, (test_count == 1) ? "test" : "tests",
                                    testcase_count, (testcase_count == 1) ? "test case" : "test cases");
  if (testing::GTEST_FLAG(print_time)) {
    printf(" (%" PRId64 " ms total)", elapsed_time_ns / 1000000);
  }
  printf("\n");
  ColoredPrintf(COLOR_GREEN,  "[   PASS   ] ");
  printf("%zu %s.\n", success_test_count, (success_test_count == 1) ? "test" : "tests");

  // Print tests failed.
  size_t fail_test_count = fail_test_name_list.size();
  if (fail_test_count > 0) {
    ColoredPrintf(COLOR_RED,  "[   FAIL   ] ");
    printf("%zu %s, listed below:\n", fail_test_count, (fail_test_count == 1) ? "test" : "tests");
    for (const auto& name : fail_test_name_list) {
      ColoredPrintf(COLOR_RED, "[   FAIL   ] ");
      printf("%s\n", name.c_str());
    }
  }

  // Print tests run timeout.
  size_t timeout_test_count = timeout_test_list.size();
  if (timeout_test_count > 0) {
    ColoredPrintf(COLOR_RED, "[ TIMEOUT  ] ");
    printf("%zu %s, listed below:\n", timeout_test_count, (timeout_test_count == 1) ? "test" : "tests");
    for (const auto& timeout_pair : timeout_test_list) {
      ColoredPrintf(COLOR_RED, "[ TIMEOUT  ] ");
      printf("%s (stopped at %" PRId64 " ms)\n", timeout_pair.first.c_str(),
                                                 timeout_pair.second / 1000000);
    }
  }

  // Print tests run exceed warnline.
  size_t slow_test_count = slow_test_list.size();
  if (slow_test_count > 0) {
    ColoredPrintf(COLOR_YELLOW, "[   SLOW   ] ");
    printf("%zu %s, listed below:\n", slow_test_count, (slow_test_count == 1) ? "test" : "tests");
    for (const auto& slow_tuple : slow_test_list) {
      ColoredPrintf(COLOR_YELLOW, "[   SLOW   ] ");
      printf("%s (%" PRId64 " ms, exceed warnline %d ms)\n", std::get<0>(slow_tuple).c_str(),
             std::get<1>(slow_tuple) / 1000000, std::get<2>(slow_tuple));
    }
  }

  if (fail_test_count > 0) {
    printf("\n%2zu FAILED %s\n", fail_test_count, (fail_test_count == 1) ? "TEST" : "TESTS");
  }
  if (timeout_test_count > 0) {
    printf("%2zu TIMEOUT %s\n", timeout_test_count, (timeout_test_count == 1) ? "TEST" : "TESTS");
  }
  if (slow_test_count > 0) {
    printf("%2zu SLOW %s\n", slow_test_count, (slow_test_count == 1) ? "TEST" : "TESTS");
  }
  fflush(stdout);
}

// Output xml file when --gtest_output is used, write this function as we can't reuse
// gtest.cc:XmlUnitTestResultPrinter. The reason is XmlUnitTestResultPrinter is totally
// defined in gtest.cc and not expose to outside. What's more, as we don't run gtest in
// the parent process, we don't have gtest classes which are needed by XmlUnitTestResultPrinter.
void OnTestIterationEndXmlPrint(const std::string& xml_output_filename,
                                const std::vector<TestCase>& testcase_list,
                                time_t epoch_iteration_start_time,
                                int64_t elapsed_time_ns) {
  FILE* fp = fopen(xml_output_filename.c_str(), "w");
  if (fp == NULL) {
    fprintf(stderr, "failed to open '%s': %s\n", xml_output_filename.c_str(), strerror(errno));
    exit(1);
  }

  size_t total_test_count = 0;
  size_t total_failed_count = 0;
  std::vector<size_t> failed_count_list(testcase_list.size(), 0);
  std::vector<int64_t> elapsed_time_list(testcase_list.size(), 0);
  for (size_t i = 0; i < testcase_list.size(); ++i) {
    auto& testcase = testcase_list[i];
    total_test_count += testcase.TestCount();
    for (size_t j = 0; j < testcase.TestCount(); ++j) {
      if (testcase.GetTestResult(j) != TEST_SUCCESS) {
        ++failed_count_list[i];
      }
      elapsed_time_list[i] += testcase.GetTestTime(j);
    }
    total_failed_count += failed_count_list[i];
  }

  const tm* time_struct = localtime(&epoch_iteration_start_time);
  char timestamp[40];
  snprintf(timestamp, sizeof(timestamp), "%4d-%02d-%02dT%02d:%02d:%02d",
           time_struct->tm_year + 1900, time_struct->tm_mon + 1, time_struct->tm_mday,
           time_struct->tm_hour, time_struct->tm_min, time_struct->tm_sec);

  fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", fp);
  fprintf(fp, "<testsuites tests=\"%zu\" failures=\"%zu\" disabled=\"0\" errors=\"0\"",
          total_test_count, total_failed_count);
  fprintf(fp, " timestamp=\"%s\" time=\"%.3lf\" name=\"AllTests\">\n", timestamp, elapsed_time_ns / 1e9);
  for (size_t i = 0; i < testcase_list.size(); ++i) {
    auto& testcase = testcase_list[i];
    fprintf(fp, "  <testsuite name=\"%s\" tests=\"%zu\" failures=\"%zu\" disabled=\"0\" errors=\"0\"",
            testcase.GetName().c_str(), testcase.TestCount(), failed_count_list[i]);
    fprintf(fp, " time=\"%.3lf\">\n", elapsed_time_list[i] / 1e9);

    for (size_t j = 0; j < testcase.TestCount(); ++j) {
      fprintf(fp, "    <testcase name=\"%s\" status=\"run\" time=\"%.3lf\" classname=\"%s\"",
              testcase.GetTest(j).GetName().c_str(), testcase.GetTestTime(j) / 1e9,
              testcase.GetName().c_str());
      if (testcase.GetTestResult(j) == TEST_SUCCESS) {
        fputs(" />\n", fp);
      } else {
        fputs(">\n", fp);
        const std::string& failure_message = testcase.GetTest(j).GetFailureMessage();
        fprintf(fp, "      <failure message=\"%s\" type=\"\">\n", failure_message.c_str());
        fputs("      </failure>\n", fp);
        fputs("    </testcase>\n", fp);
      }
    }

    fputs("  </testsuite>\n", fp);
  }
  fputs("</testsuites>\n", fp);
  fclose(fp);
}

// Forked Child process, run the single test.
static void ChildProcessFn(int argc, char** argv, const std::string& test_name) {
  char** new_argv = new char*[argc + 2];
  memcpy(new_argv, argv, sizeof(char*) * argc);

  char* filter_arg = new char [test_name.size() + 20];
  strcpy(filter_arg, "--gtest_filter=");
  strcat(filter_arg, test_name.c_str());
  new_argv[argc] = filter_arg;
  new_argv[argc + 1] = NULL;

  int new_argc = argc + 1;
  testing::InitGoogleTest(&new_argc, new_argv);
  int result = RUN_ALL_TESTS();
  exit(result);
}

struct ChildProcInfo {
  pid_t pid;
  int64_t start_time_ns;
  int64_t deadline_time_ns;
  size_t testcase_id, test_id;
  bool done_flag;
  bool timeout_flag;
  int exit_status;
  int child_read_fd;
  ChildProcInfo() : pid(0) {}
};

static void WaitChildProcs(std::vector<ChildProcInfo>& child_proc_list) {
  pid_t result;
  int status;
  bool loop_flag = true;

  while (true) {
    while ((result = waitpid(-1, &status, WNOHANG)) == -1) {
      if (errno != EINTR) {
        break;
      }
    }

    if (result == -1) {
      perror("waitpid");
      exit(1);
    } else if (result == 0) {
      // Check child timeout.
      int64_t current_time_ns = NanoTime();
      for (size_t i = 0; i < child_proc_list.size(); ++i) {
        if (child_proc_list[i].deadline_time_ns <= current_time_ns) {
          child_proc_list[i].done_flag = true;
          child_proc_list[i].timeout_flag = true;
          loop_flag = false;
        }
      }
    } else {
      // Check child finish.
      for (size_t i = 0; i < child_proc_list.size(); ++i) {
        if (child_proc_list[i].pid == result) {
          child_proc_list[i].done_flag = true;
          child_proc_list[i].timeout_flag = false;
          child_proc_list[i].exit_status = status;
          loop_flag = false;
          break;
        }
      }
    }

    if (!loop_flag) break;
    // sleep 1 ms to avoid busy looping.
    timespec sleep_time;
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 1000000;
    nanosleep(&sleep_time, NULL);
  }
}

static TestResult WaitChildProc(pid_t pid) {
  pid_t result;
  int exit_status;

  while ((result = waitpid(pid, &exit_status, 0)) == -1) {
    if (errno != EINTR) {
      break;
    }
  }

  TestResult test_result = TEST_SUCCESS;
  if (result != pid || WEXITSTATUS(exit_status) != 0) {
    test_result = TEST_FAILED;
  }
  return test_result;
}

// We choose to use multi-fork and multi-wait here instead of multi-thread, because it always
// makes deadlock to use fork in multi-thread.
static void RunTestInSeparateProc(int argc, char** argv, std::vector<TestCase>& testcase_list,
                                  size_t iteration_count, size_t job_count,
                                  const std::string& xml_output_filename) {
  // Stop default result printer to avoid environment setup/teardown information for each test.
  testing::UnitTest::GetInstance()->listeners().Release(
                        testing::UnitTest::GetInstance()->listeners().default_result_printer());
  testing::UnitTest::GetInstance()->listeners().Append(new TestResultPrinter);

  for (size_t iteration = 1; iteration <= iteration_count; ++iteration) {
    OnTestIterationStartPrint(testcase_list, iteration, iteration_count);
    int64_t iteration_start_time_ns = NanoTime();
    time_t epoch_iteration_start_time = time(NULL);

    // Run up to job_count tests in parallel, each test in a child process.
    std::vector<ChildProcInfo> child_proc_list(job_count);

    // Next test to run is [next_testcase_id:next_test_id].
    size_t next_testcase_id = 0;
    size_t next_test_id = 0;

    // Record how many tests are finished.
    std::vector<size_t> finished_test_count_list(testcase_list.size(), 0);
    size_t finished_testcase_count = 0;

    while (finished_testcase_count < testcase_list.size()) {
      // Fork up to job_count child processes.
      for (auto& child_proc : child_proc_list) {
        if (child_proc.pid == 0 && next_testcase_id < testcase_list.size()) {
          std::string test_name = testcase_list[next_testcase_id].GetTestName(next_test_id);
          int pipefd[2];
          int ret = pipe(pipefd);
          if (ret == -1) {
            perror("pipe2 in RunTestInSeparateProc");
            exit(1);
          }
          pid_t pid = fork();
          if (pid == -1) {
            perror("fork in RunTestInSeparateProc");
            exit(1);
          } else if (pid == 0) {
            close(pipefd[0]);
            child_output_fd = pipefd[1];
            // Run child process test, never return.
            ChildProcessFn(argc, argv, test_name);
          }
          // Parent process
          close(pipefd[1]);
          child_proc.child_read_fd = pipefd[0];
          child_proc.pid = pid;
          child_proc.start_time_ns = NanoTime();
          child_proc.deadline_time_ns = child_proc.start_time_ns +
                                        GetDeadlineInfo(test_name) * 1000000LL;
          child_proc.testcase_id = next_testcase_id;
          child_proc.test_id = next_test_id;
          child_proc.done_flag = false;
          if (++next_test_id == testcase_list[next_testcase_id].TestCount()) {
            next_test_id = 0;
            ++next_testcase_id;
          }
        }
      }

      // Wait for any child proc finish or timeout.
      WaitChildProcs(child_proc_list);

      // Collect result.
      for (auto& child_proc : child_proc_list) {
        if (child_proc.pid != 0 && child_proc.done_flag == true) {
          size_t testcase_id = child_proc.testcase_id;
          size_t test_id = child_proc.test_id;
          TestCase& testcase = testcase_list[testcase_id];
          testcase.SetTestTime(test_id, NanoTime() - child_proc.start_time_ns);

          // Kill and wait the timeout child process before we read failure message.
          if (child_proc.timeout_flag) {
            kill(child_proc.pid, SIGKILL);
            WaitChildProc(child_proc.pid);
          }

          while (true) {
            char buf[1024];
            int ret = TEMP_FAILURE_RETRY(read(child_proc.child_read_fd, buf, sizeof(buf) - 1));
            if (ret > 0) {
              buf[ret] = '\0';
              testcase.GetTest(test_id).AppendFailureMessage(buf);
            } else if (ret == 0) {
              break; // Read end.
            } else {
              perror("read child_read_fd in RunTestInSeparateProc");
              exit(1);
            }
          }
          close(child_proc.child_read_fd);

          if (child_proc.timeout_flag) {
            testcase.SetTestResult(test_id, TEST_TIMEOUT);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s killed because of timeout at %" PRId64 " ms.\n",
                     testcase.GetTestName(test_id).c_str(),
                     testcase.GetTestTime(test_id) / 1000000);
            testcase.GetTest(test_id).AppendFailureMessage(buf);

          } else if (WIFSIGNALED(child_proc.exit_status)) {
            // Record signal terminated test as failed.
            testcase.SetTestResult(test_id, TEST_FAILED);
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s terminated by signal: %s.\n",
                     testcase.GetTestName(test_id).c_str(),
                     strsignal(WTERMSIG(child_proc.exit_status)));
            testcase.GetTest(test_id).AppendFailureMessage(buf);

          } else {
            testcase.SetTestResult(test_id, WEXITSTATUS(child_proc.exit_status) == 0 ?
                                   TEST_SUCCESS : TEST_FAILED);
          }
          OnTestEndPrint(testcase, test_id);

          if (++finished_test_count_list[testcase_id] == testcase.TestCount()) {
            ++finished_testcase_count;
          }
          child_proc.pid = 0;
          child_proc.done_flag = false;
        }
      }
    }

    int64_t elapsed_time_ns = NanoTime() - iteration_start_time_ns;
    OnTestIterationEndPrint(testcase_list, iteration, elapsed_time_ns);
    if (!xml_output_filename.empty()) {
      OnTestIterationEndXmlPrint(xml_output_filename, testcase_list, epoch_iteration_start_time,
                                 elapsed_time_ns);
    }
  }
}

static size_t GetProcessorCount() {
  return static_cast<size_t>(sysconf(_SC_NPROCESSORS_ONLN));
}

static void AddGtestFilterSynonym(std::vector<char*>& args) {
  // Support --gtest-filter as a synonym for --gtest_filter.
  for (size_t i = 1; i < args.size(); ++i) {
    if (strncmp(args[i], "--gtest-filter", strlen("--gtest-filter")) == 0) {
      args[i][7] = '_';
    }
  }
}

struct IsolationTestOptions {
  bool isolate;
  size_t job_count;
  int test_deadline_ms;
  int test_warnline_ms;
  std::string gtest_color;
  bool gtest_print_time;
  size_t gtest_repeat;
  std::string gtest_output;
};

// Pick options not for gtest: There are two parts in args, one part is used in isolation test mode
// as described in PrintHelpInfo(), the other part is handled by testing::InitGoogleTest() in
// gtest. PickOptions() picks the first part into IsolationTestOptions structure, leaving the second
// part in args.
// Arguments:
//   args is used to pass in all command arguments, and pass out only the part of options for gtest.
//   options is used to pass out test options in isolation mode.
// Return false if there is error in arguments.
static bool PickOptions(std::vector<char*>& args, IsolationTestOptions& options) {
  for (size_t i = 1; i < args.size(); ++i) {
    if (strcmp(args[i], "--help") == 0 || strcmp(args[i], "-h") == 0) {
      PrintHelpInfo();
      options.isolate = false;
      return true;
    }
  }

  AddGtestFilterSynonym(args);

  // if --bionic-selftest argument is used, only enable self tests, otherwise remove self tests.
  bool enable_selftest = false;
  for (size_t i = 1; i < args.size(); ++i) {
    if (strcmp(args[i], "--bionic-selftest") == 0) {
      // This argument is to enable "bionic_selftest*" for self test, and is not shown in help info.
      // Don't remove this option from arguments.
      enable_selftest = true;
    }
  }
  std::string gtest_filter_str;
  for (size_t i = args.size() - 1; i >= 1; --i) {
    if (strncmp(args[i], "--gtest_filter=", strlen("--gtest_filter=")) == 0) {
      gtest_filter_str = std::string(args[i]);
      args.erase(args.begin() + i);
      break;
    }
  }
  if (enable_selftest == true) {
    args.push_back(strdup("--gtest_filter=bionic_selftest*"));
  } else {
    if (gtest_filter_str == "") {
      gtest_filter_str = "--gtest_filter=-bionic_selftest*";
    } else {
      // Find if '-' for NEGATIVE_PATTERNS exists.
      if (gtest_filter_str.find(":-") != std::string::npos) {
        gtest_filter_str += ":bionic_selftest*";
      } else {
        gtest_filter_str += ":-bionic_selftest*";
      }
    }
    args.push_back(strdup(gtest_filter_str.c_str()));
  }

  options.isolate = true;
  // Parse arguments that make us can't run in isolation mode.
  for (size_t i = 1; i < args.size(); ++i) {
    if (strcmp(args[i], "--no-isolate") == 0) {
      options.isolate = false;
    } else if (strcmp(args[i], "--gtest_list_tests") == 0) {
      options.isolate = false;
    }
  }

  // Stop parsing if we will not run in isolation mode.
  if (options.isolate == false) {
    return true;
  }

  // Init default isolation test options.
  options.job_count = GetProcessorCount();
  options.test_deadline_ms = DEFAULT_GLOBAL_TEST_RUN_DEADLINE_MS;
  options.test_warnline_ms = DEFAULT_GLOBAL_TEST_RUN_WARNLINE_MS;
  options.gtest_color = testing::GTEST_FLAG(color);
  options.gtest_print_time = testing::GTEST_FLAG(print_time);
  options.gtest_repeat = testing::GTEST_FLAG(repeat);
  options.gtest_output = testing::GTEST_FLAG(output);

  // Parse arguments speficied for isolation mode.
  for (size_t i = 1; i < args.size(); ++i) {
    if (strncmp(args[i], "-j", strlen("-j")) == 0) {
      char* p = args[i] + strlen("-j");
      int count = 0;
      if (*p != '\0') {
        // Argument like -j5.
        count = atoi(p);
      } else if (args.size() > i + 1) {
        // Arguments like -j 5.
        count = atoi(args[i + 1]);
        ++i;
      }
      if (count <= 0) {
        fprintf(stderr, "invalid job count: %d\n", count);
        return false;
      }
      options.job_count = static_cast<size_t>(count);
    } else if (strncmp(args[i], "--deadline=", strlen("--deadline=")) == 0) {
      int time_ms = atoi(args[i] + strlen("--deadline="));
      if (time_ms <= 0) {
        fprintf(stderr, "invalid deadline: %d\n", time_ms);
        return false;
      }
      options.test_deadline_ms = time_ms;
    } else if (strncmp(args[i], "--warnline=", strlen("--warnline=")) == 0) {
      int time_ms = atoi(args[i] + strlen("--warnline="));
      if (time_ms <= 0) {
        fprintf(stderr, "invalid warnline: %d\n", time_ms);
        return false;
      }
      options.test_warnline_ms = time_ms;
    } else if (strncmp(args[i], "--gtest_color=", strlen("--gtest_color=")) == 0) {
      options.gtest_color = args[i] + strlen("--gtest_color=");
    } else if (strcmp(args[i], "--gtest_print_time=0") == 0) {
      options.gtest_print_time = false;
    } else if (strncmp(args[i], "--gtest_repeat=", strlen("--gtest_repeat=")) == 0) {
      int repeat = atoi(args[i] + strlen("--gtest_repeat="));
      if (repeat < 0) {
        fprintf(stderr, "invalid gtest_repeat count: %d\n", repeat);
        return false;
      }
      options.gtest_repeat = repeat;
      // Remove --gtest_repeat=xx from arguments, so child process only run one iteration for a single test.
      args.erase(args.begin() + i);
      --i;
    } else if (strncmp(args[i], "--gtest_output=", strlen("--gtest_output=")) == 0) {
      std::string output = args[i] + strlen("--gtest_output=");
      // generate output xml file path according to the strategy in gtest.
      bool success = true;
      if (strncmp(output.c_str(), "xml:", strlen("xml:")) == 0) {
        output = output.substr(strlen("xml:"));
        if (output.size() == 0) {
          success = false;
        }
        // Make absolute path.
        if (success && output[0] != '/') {
          char* cwd = getcwd(NULL, 0);
          if (cwd != NULL) {
            output = std::string(cwd) + "/" + output;
            free(cwd);
          } else {
            success = false;
          }
        }
        // Add file name if output is a directory.
        if (success && output.back() == '/') {
          output += "test_details.xml";
        }
      }
      if (success) {
        options.gtest_output = output;
      } else {
        fprintf(stderr, "invalid gtest_output file: %s\n", args[i]);
        return false;
      }

      // Remove --gtest_output=xxx from arguments, so child process will not write xml file.
      args.erase(args.begin() + i);
      --i;
    }
  }

  // Add --no-isolate in args to prevent child process from running in isolation mode again.
  // As DeathTest will try to call execve(), this argument should always be added.
  args.insert(args.begin() + 1, strdup("--no-isolate"));
  return true;
}

int main(int argc, char** argv) {
  std::vector<char*> arg_list;
  for (int i = 0; i < argc; ++i) {
    arg_list.push_back(argv[i]);
  }

  IsolationTestOptions options;
  if (PickOptions(arg_list, options) == false) {
    return 1;
  }

  if (options.isolate == true) {
    // Set global variables.
    global_test_run_deadline_ms = options.test_deadline_ms;
    global_test_run_warnline_ms = options.test_warnline_ms;
    testing::GTEST_FLAG(color) = options.gtest_color.c_str();
    testing::GTEST_FLAG(print_time) = options.gtest_print_time;
    std::vector<TestCase> testcase_list;

    argc = static_cast<int>(arg_list.size());
    arg_list.push_back(NULL);
    if (EnumerateTests(argc, arg_list.data(), testcase_list) == false) {
      return 1;
    }
    RunTestInSeparateProc(argc, arg_list.data(), testcase_list, options.gtest_repeat,
                          options.job_count, options.gtest_output);
  } else {
    argc = static_cast<int>(arg_list.size());
    arg_list.push_back(NULL);
    testing::InitGoogleTest(&argc, arg_list.data());
    return RUN_ALL_TESTS();
  }
  return 0;
}

//################################################################################
// Bionic Gtest self test, run this by --bionic-selftest option.

TEST(bionic_selftest, test_success) {
  ASSERT_EQ(1, 1);
}

TEST(bionic_selftest, test_fail) {
  ASSERT_EQ(0, 1);
}

TEST(bionic_selftest, test_time_warn) {
  sleep(4);
}

TEST(bionic_selftest, test_timeout) {
  while (1) {}
}

TEST(bionic_selftest, test_signal_SEGV_terminated) {
  char* p = reinterpret_cast<char*>(static_cast<intptr_t>(atoi("0")));
  *p = 3;
}

class bionic_selftest_DeathTest : public BionicDeathTest {};

static void deathtest_helper_success() {
  ASSERT_EQ(1, 1);
  exit(0);
}

TEST_F(bionic_selftest_DeathTest, success) {
  ASSERT_EXIT(deathtest_helper_success(), ::testing::ExitedWithCode(0), "");
}

static void deathtest_helper_fail() {
  ASSERT_EQ(1, 0);
}

TEST_F(bionic_selftest_DeathTest, fail) {
  ASSERT_EXIT(deathtest_helper_fail(), ::testing::ExitedWithCode(0), "");
}