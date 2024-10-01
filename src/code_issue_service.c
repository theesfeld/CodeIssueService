#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <errno.h>
#include <getopt.h>
#include <git2.h>
#include <hiredis/hiredis.h>
#include <ini.h>
#include <microhttpd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define MAX_BUFFER_SIZE 8192

// Configure logging
void configure_logging() {
  openlog("code_issue_service", LOG_PID | LOG_CONS, LOG_USER);
  setlogmask(LOG_UPTO(LOG_INFO));
}

// Function prototypes
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                           void *userp);
int process_issue(const char *repo_owner, const char *repo_name,
                  int issue_number, const char *issue_title,
                  const char *issue_body);
int analyze_issue(const char *repo_owner, const char *repo_name,
                  int issue_number, const char *issue_body, char *response);
int implement_issue(const char *repo_owner, const char *repo_name,
                    int issue_number, const char *branch_name, char *response);
int review_changes(const char *repo_owner, const char *repo_name,
                   int issue_number, const char *branch_name, char *response);
int final_review(const char *repo_owner, const char *repo_name,
                 int issue_number, const char *branch_name, char *response);
int create_pr(const char *repo_owner, const char *repo_name, int issue_number,
              const char *branch_name, char *response);
enum MHD_Result answer_to_connection(void *cls,
                                     struct MHD_Connection *connection,
                                     const char *url, const char *method,
                                     const char *version,
                                     const char *upload_data,
                                     size_t *upload_data_size, void **con_cls);

// Configuration variables
int SERVER_PORT = 8080;
char LOG_DIRECTORY[256] = "./logs";
char REDIS_HOST[256] = "127.0.0.1";
int REDIS_PORT = 6379;
char GITHUB_TOKEN[128] = "";
char AI_PROVIDER[32] = "openai";
char AI_API_KEY[128] = "";
char AI_MODEL[64] = "text-davinci-003";

// Prompts
char ANALYZE_PROMPT_TEMPLATE[MAX_BUFFER_SIZE];
char IMPLEMENT_PROMPT_TEMPLATE[MAX_BUFFER_SIZE];
char REVIEW_PROMPT_TEMPLATE[MAX_BUFFER_SIZE];
char FINAL_REVIEW_PROMPT_TEMPLATE[MAX_BUFFER_SIZE];
char PR_PROMPT_TEMPLATE[MAX_BUFFER_SIZE];

// Function to load config file
int config_handler(void *user, const char *section, const char *name,
                   const char *value) {
  (void)user; // Suppress unused parameter warning
  if (strcmp(section, "Server") == 0) {
    if (strcmp(name, "port") == 0) {
      SERVER_PORT = atoi(value);
    } else if (strcmp(name, "log_directory") == 0) {
      strcpy(LOG_DIRECTORY, value);
    } else if (strcmp(name, "redis_host") == 0) {
      strcpy(REDIS_HOST, value);
    } else if (strcmp(name, "redis_port") == 0) {
      REDIS_PORT = atoi(value);
    }
  } else if (strcmp(section, "GitHub") == 0) {
    if (strcmp(name, "personal_access_token") == 0) {
      strcpy(GITHUB_TOKEN, value);
    }
  } else if (strcmp(section, "AI") == 0) {
    if (strcmp(name, "api_provider") == 0) {
      strcpy(AI_PROVIDER, value);
    } else if (strcmp(name, "api_key") == 0) {
      strcpy(AI_API_KEY, value);
    } else if (strcmp(name, "model") == 0) {
      strcpy(AI_MODEL, value);
    }
  } else if (strcmp(section, "Prompts") == 0) {
    if (strcmp(name, "analyze_issue_prompt") == 0) {
      strcpy(ANALYZE_PROMPT_TEMPLATE, value);
    } else if (strcmp(name, "implement_changes_prompt") == 0) {
      strcpy(IMPLEMENT_PROMPT_TEMPLATE, value);
    } else if (strcmp(name, "review_changes_prompt") == 0) {
      strcpy(REVIEW_PROMPT_TEMPLATE, value);
    } else if (strcmp(name, "final_review_prompt") == 0) {
      strcpy(FINAL_REVIEW_PROMPT_TEMPLATE, value);
    } else if (strcmp(name, "create_pr_prompt") == 0) {
      strcpy(PR_PROMPT_TEMPLATE, value);
    }
  }
  return 1;
}

// Logging function
void log_message(int issue_number, const char *format, ...) {
  char log_file_path[512];
  snprintf(log_file_path, sizeof(log_file_path), "%s/issue_%d.log",
           LOG_DIRECTORY, issue_number);

  FILE *log_file = fopen(log_file_path, "a");
  if (!log_file) {
    syslog(LOG_ERR, "Failed to open log file: %s", log_file_path);
    return;
  }

  va_list args;
  va_start(args, format);

  time_t now = time(NULL);
  char time_str[64];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

  fprintf(log_file, "[%s] ", time_str);
  vfprintf(log_file, format, args);
  fprintf(log_file, "\n");

  va_end(args);
  fclose(log_file);
}

// Structure to hold memory for curl callback
struct MemoryStruct {
  char *memory;
  size_t size;
};

// Callback function for curl to write data
size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                           void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;

  mem->memory = realloc(mem->memory, mem->size + realsize + 1);
  if (mem->memory == NULL) {
    syslog(LOG_ERR, "Not enough memory (realloc returned NULL)");
    return 0;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

// Function to send a POST request to AI API for code analysis or generation
int send_ai_request(const char *prompt, char *response) {
  CURL *curl;
  CURLcode res;
  struct curl_slist *headers = NULL;
  char buffer[MAX_BUFFER_SIZE];
  struct MemoryStruct chunk;

  chunk.memory = malloc(1); // Will be grown as needed by realloc
  chunk.size = 0;           // No data at this point

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();
  if (curl) {
    // Prepare the API request depending on the provider
    if (strcmp(AI_PROVIDER, "openai") == 0) {
      snprintf(buffer, sizeof(buffer),
               "{\"model\":\"%s\",\"prompt\":\"%s\",\"max_tokens\":1000,"
               "\"temperature\":0.7}",
               AI_MODEL, prompt);
      curl_easy_setopt(curl, CURLOPT_URL,
                       "https://api.openai.com/v1/completions");
    } else if (strcmp(AI_PROVIDER, "anthropic") == 0) {
      snprintf(buffer, sizeof(buffer),
               "{\"prompt\":\"%s\", \"model\":\"%s\", "
               "\"max_tokens_to_sample\":1000, \"temperature\":0.7}",
               prompt, AI_MODEL);
      curl_easy_setopt(curl, CURLOPT_URL,
                       "https://api.anthropic.com/v1/complete");
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
             AI_API_KEY);
    headers = curl_slist_append(headers, auth_header);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buffer);

    // Set up response handling
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      syslog(LOG_ERR, "Curl failed: %s", curl_easy_strerror(res));
      free(chunk.memory);
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return -1;
    }

    // Copy response
    strncpy(response, chunk.memory, MAX_BUFFER_SIZE - 1);
    response[MAX_BUFFER_SIZE - 1] = '\0';

    free(chunk.memory);
    curl_easy_cleanup(curl);
  }
  curl_global_cleanup();
  return 0;
}

// Function to enqueue issue in Redis
int enqueue_issue(redisContext *redis_ctx, const char *issue_data) {
  redisReply *reply =
      redisCommand(redis_ctx, "RPUSH issue_queue %s", issue_data);
  if (!reply) {
    syslog(LOG_ERR, "Failed to enqueue issue in Redis");
    return -1;
  }
  freeReplyObject(reply);
  return 0;
}

// Function to dequeue issue from Redis
char *dequeue_issue(redisContext *redis_ctx) {
  redisReply *reply = redisCommand(redis_ctx, "LPOP issue_queue");
  if (!reply) {
    syslog(LOG_ERR, "Failed to dequeue issue from Redis");
    return NULL;
  }
  if (reply->type == REDIS_REPLY_NIL) {
    freeReplyObject(reply);
    return NULL; // No issues in queue
  }
  char *issue_data = NULL;
  if (reply->str) {
    issue_data = strdup(reply->str);
    if (!issue_data) {
      syslog(LOG_ERR, "Failed to duplicate issue data");
    }
  } else {
    syslog(LOG_ERR, "Received NULL string from Redis");
  }
  freeReplyObject(reply);
  return issue_data;
}

// Function to process an issue (to be run in a separate thread)
void *process_issue_thread(void *arg) {
  redisContext *redis_ctx = (redisContext *)arg;
  syslog(LOG_INFO, "Issue processing thread started");
  while (1) {
    char *issue_data = dequeue_issue(redis_ctx);
    if (!issue_data) {
      sleep(1); // Wait before checking the queue again
      continue;
    }
    syslog(LOG_INFO, "Dequeued new issue for processing: %s", issue_data);

    // Parse issue data
    cJSON *issue_json = cJSON_Parse(issue_data);
    if (!issue_json) {
      syslog(LOG_ERR, "Failed to parse issue data: %s", issue_data);
      free(issue_data);
      continue;
    }

    cJSON *repo_item = cJSON_GetObjectItem(issue_json, "repository");
    cJSON *issue_number_item = cJSON_GetObjectItem(issue_json, "issue_number");
    cJSON *issue_title_item = cJSON_GetObjectItem(issue_json, "issue_title");
    cJSON *issue_body_item = cJSON_GetObjectItem(issue_json, "issue_body");

    if (!repo_item || !issue_number_item || !issue_title_item ||
        !issue_body_item) {
      syslog(LOG_ERR, "Invalid issue data");
      cJSON_Delete(issue_json);
      free(issue_data);
      continue;
    }

    const char *repo_full_name = repo_item->valuestring;
    int issue_number = issue_number_item->valueint;
    const char *issue_title = issue_title_item->valuestring;
    const char *issue_body = issue_body_item->valuestring;

    // Split repo_full_name into owner and repo
    char repo_owner[128], repo_name[128];
    sscanf(repo_full_name, "%[^/]/%s", repo_owner, repo_name);

    log_message(issue_number, "Processing issue #%d in repository %s/%s",
                issue_number, repo_owner, repo_name);

    // Process the issue
    int result = process_issue(repo_owner, repo_name, issue_number, issue_title,
                               issue_body);
    if (result == 0) {
      log_message(issue_number, "Successfully processed issue #%d",
                  issue_number);
    } else {
      log_message(issue_number, "Failed to process issue #%d", issue_number);
    }

    cJSON_Delete(issue_json);
    free(issue_data); // Free issue_data after we're done using it
  }
  return NULL;
}

// Function to clone the repository
int clone_repository(const char *repo_owner, const char *repo_name,
                     const char *local_path, int issue_number) {
  git_libgit2_init();

  char repo_url[256];
  snprintf(repo_url, sizeof(repo_url), "https://github.com/%s/%s.git",
           repo_owner, repo_name);

  git_repository *repo = NULL;
  int error = git_clone(&repo, repo_url, local_path, NULL);
  if (error != 0) {
    const git_error *e = git_error_last();
    log_message(issue_number, "Error cloning repository: %s", e->message);
    git_libgit2_shutdown();
    return -1;
  }

  git_repository_free(repo);
  git_libgit2_shutdown();
  return 0;
}

// Function to create and checkout a new branch
int create_and_checkout_branch(const char *branch_name, const char *local_path,
                               int issue_number) {
  git_libgit2_init();

  git_repository *repo = NULL;
  git_reference *head_ref = NULL;
  git_object *head_commit = NULL;
  git_reference *new_branch_ref = NULL;

  int error = git_repository_open(&repo, local_path);
  if (error != 0) {
    log_message(issue_number, "Error opening repository: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_reference_lookup(&head_ref, repo, "HEAD");
  if (error != 0) {
    log_message(issue_number, "Error looking up HEAD: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_reference_peel(&head_commit, head_ref, GIT_OBJECT_COMMIT);
  if (error != 0) {
    log_message(issue_number, "Error peeling HEAD to commit: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_branch_create(&new_branch_ref, repo, branch_name,
                            (git_commit *)head_commit, 0);
  if (error != 0) {
    log_message(issue_number, "Error creating new branch: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_checkout_tree(repo, (git_object *)head_commit, NULL);
  if (error != 0) {
    log_message(issue_number, "Error checking out tree: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_repository_set_head(repo, git_reference_name(new_branch_ref));
  if (error != 0) {
    log_message(issue_number, "Error setting HEAD to new branch: %s",
                git_error_last()->message);
    goto cleanup;
  }

cleanup:
  if (head_ref)
    git_reference_free(head_ref);
  if (head_commit)
    git_object_free(head_commit);
  if (new_branch_ref)
    git_reference_free(new_branch_ref);
  if (repo)
    git_repository_free(repo);
  git_libgit2_shutdown();

  if (error != 0) {
    return -1;
  }

  return 0;
}

int apply_code_changes(const char *local_path, const char *ai_response,
                       int issue_number) {
  // Parse AI response and apply changes
  cJSON *json = cJSON_Parse(ai_response);
  if (!json) {
    log_message(issue_number, "Error parsing AI response");
    return -1;
  }

  cJSON *changes = cJSON_GetObjectItem(json, "changes");
  if (!cJSON_IsArray(changes)) {
    log_message(issue_number,
                "Invalid AI response format: 'changes' is not an array");
    cJSON_Delete(json);
    return -1;
  }

  cJSON *change = NULL;
  cJSON_ArrayForEach(change, changes) {
    cJSON *file_item = cJSON_GetObjectItem(change, "file");
    cJSON *content_item = cJSON_GetObjectItem(change, "content");

    if (!cJSON_IsString(file_item) || !cJSON_IsString(content_item)) {
      log_message(issue_number, "Invalid change format in AI response");
      continue;
    }

    const char *file_path = file_item->valuestring;
    const char *file_content = content_item->valuestring;

    // Construct the full path to the file
    char full_file_path[512];
    snprintf(full_file_path, sizeof(full_file_path), "%s/%s", local_path,
             file_path);

    // Ensure the file path is within the local_path directory to prevent
    // directory traversal attacks
    char resolved_local_path[PATH_MAX];
    char resolved_full_file_path[PATH_MAX];
    realpath(local_path, resolved_local_path);
    realpath(full_file_path, resolved_full_file_path);

    if (strstr(resolved_full_file_path, resolved_local_path) !=
        resolved_full_file_path) {
      log_message(issue_number, "Invalid file path in AI response: %s",
                  full_file_path);
      continue;
    }

    // Create directories if necessary
    char dir_path[512];
    strncpy(dir_path, full_file_path, sizeof(dir_path));
    dir_path[sizeof(dir_path) - 1] = '\0';
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
      *last_slash = '\0';
      struct stat st = {0};
      if (stat(dir_path, &st) == -1) {
        if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
          log_message(issue_number, "Failed to create directory: %s", dir_path);
          continue;
        }
      }
    }

    // Write content to the file
    FILE *file = fopen(full_file_path, "w");
    if (!file) {
      log_message(issue_number, "Failed to open file for writing: %s",
                  full_file_path);
      continue;
    }

    if (fprintf(file, "%s", file_content) < 0) {
      log_message(issue_number, "Failed to write content to file: %s",
                  full_file_path);
      fclose(file);
      continue;
    }

    fclose(file);
    log_message(issue_number, "Applied changes to file: %s", full_file_path);
  }

  cJSON_Delete(json);
  return 0;
}
// Function to commit and push changes to GitHub
int commit_and_push_changes(const char *local_path, const char *branch_name,
                            const char *commit_message, int issue_number) {
  git_libgit2_init();

  git_repository *repo = NULL;
  git_index *index = NULL;
  git_oid tree_oid, commit_oid;
  git_tree *tree = NULL;
  git_signature *signature = NULL;
  git_commit *parent_commit = NULL;
  git_remote *remote = NULL;

  int error = git_repository_open(&repo, local_path);
  if (error != 0) {
    log_message(issue_number, "Error opening repository: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_signature_now(&signature, "Automated Bot", "bot@example.com");
  if (error != 0) {
    log_message(issue_number, "Error creating signature: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_repository_index(&index, repo);
  if (error != 0) {
    log_message(issue_number, "Error getting repository index: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_index_add_all(index, NULL, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
  if (error != 0) {
    log_message(issue_number, "Error adding files to index: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_index_write(index);
  if (error != 0) {
    log_message(issue_number, "Error writing index: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_index_write_tree(&tree_oid, index);
  if (error != 0) {
    log_message(issue_number, "Error writing tree: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error = git_tree_lookup(&tree, repo, &tree_oid);
  if (error != 0) {
    log_message(issue_number, "Error looking up tree: %s",
                git_error_last()->message);
    goto cleanup;
  }

  // Get HEAD commit
  git_reference *head_ref = NULL;
  error = git_repository_head(&head_ref, repo);
  if (error != 0) {
    log_message(issue_number, "Error getting HEAD reference: %s",
                git_error_last()->message);
    goto cleanup;
  }

  error =
      git_commit_lookup(&parent_commit, repo, git_reference_target(head_ref));
  if (error != 0) {
    log_message(issue_number, "Error looking up parent commit: %s",
                git_error_last()->message);
    goto cleanup;
  }

  // Create the commit
  error = git_commit_create_v(&commit_oid, repo, "HEAD", signature, signature,
                              NULL, commit_message, tree, 1, parent_commit);
  if (error != 0) {
    log_message(issue_number, "Error creating commit: %s",
                git_error_last()->message);
    goto cleanup;
  }

  // Set up remote
  error = git_remote_lookup(&remote, repo, "origin");
  if (error != 0) {
    log_message(issue_number, "Error looking up remote: %s",
                git_error_last()->message);
    goto cleanup;
  }

  // Set up push options
  git_push_options push_opts;
  git_push_options_init(&push_opts, GIT_PUSH_OPTIONS_VERSION);

  // Push the branch
  git_strarray refspecs = {0};
  char *refspec = NULL;
  refspec = malloc(256 * sizeof(char));
  if (refspec == NULL) {
    log_message(issue_number, "Failed to allocate memory for refspec");
    goto cleanup;
  }
  snprintf(refspec, 256, "refs/heads/%s", branch_name);
  refspecs.strings = &refspec;
  refspecs.count = 1;

  error = git_remote_push(remote, &refspecs, &push_opts);
  free(refspec);
  if (error != 0) {
    log_message(issue_number, "Error pushing to remote: %s",
                git_error_last()->message);
    goto cleanup;
  }

cleanup:
  if (index)
    git_index_free(index);
  if (tree)
    git_tree_free(tree);
  if (signature)
    git_signature_free(signature);
  if (parent_commit)
    git_commit_free(parent_commit);
  if (head_ref)
    git_reference_free(head_ref);
  if (remote)
    git_remote_free(remote);
  if (repo)
    git_repository_free(repo);
  git_libgit2_shutdown();

  if (error != 0) {
    return -1;
  }

  return 0;
}

// Function to create a PR using GitHub API
int create_pull_request(const char *repo_owner, const char *repo_name,
                        int issue_number, const char *branch_name,
                        const char *pr_title, const char *pr_body) {
  CURL *curl;
  CURLcode res;
  struct curl_slist *headers = NULL;
  char url[256];
  char data[MAX_BUFFER_SIZE];
  struct MemoryStruct chunk;

  snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/pulls",
           repo_owner, repo_name);

  snprintf(data, sizeof(data),
           "{\"title\": \"%s\", \"head\": \"%s\", \"base\": \"master\", "
           "\"body\": \"%s\"}",
           pr_title, branch_name, pr_body);

  chunk.memory = malloc(1); // Will be grown as needed by realloc
  chunk.size = 0;           // No data at this point

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();
  if (curl) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Authorization: token %s",
             GITHUB_TOKEN);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "User-Agent: Automated Bot");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);

    // Set up response handling
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      log_message(issue_number, "Error creating PR: %s",
                  curl_easy_strerror(res));
      free(chunk.memory);
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return -1;
    }

    // Optionally, parse the response and log PR URL
    cJSON *json = cJSON_Parse(chunk.memory);
    if (json) {
      cJSON *html_url = cJSON_GetObjectItem(json, "html_url");
      if (html_url && cJSON_IsString(html_url)) {
        log_message(issue_number, "Pull Request created: %s",
                    html_url->valuestring);
      }
      cJSON_Delete(json);
    }

    free(chunk.memory);
    curl_easy_cleanup(curl);
  }
  curl_global_cleanup();

  return 0;
}

// Implement the AI interaction functions
int analyze_issue(const char *repo_owner, const char *repo_name,
                  int issue_number, const char *issue_body, char *response) {
  (void)repo_owner; // Suppress unused parameter warning
  (void)repo_name;  // Suppress unused parameter warning
  char prompt[MAX_BUFFER_SIZE];
  snprintf(prompt, sizeof(prompt), ANALYZE_PROMPT_TEMPLATE, issue_body);

  if (send_ai_request(prompt, response) != 0) {
    log_message(issue_number, "Failed to send AI request for issue analysis.");
    return -1;
  }

  log_message(issue_number, "Received AI response for issue analysis.");
  return 0;
}

int implement_issue(const char *repo_owner, const char *repo_name,
                    int issue_number, const char *branch_name, char *response) {
  char prompt[MAX_BUFFER_SIZE];
  snprintf(prompt, sizeof(prompt), IMPLEMENT_PROMPT_TEMPLATE, repo_owner,
           repo_name, branch_name);

  if (send_ai_request(prompt, response) != 0) {
    log_message(issue_number, "Failed to send AI request for implementation.");
    return -1;
  }

  log_message(issue_number, "Received AI response for implementation.");
  return 0;
}

int review_changes(const char *repo_owner, const char *repo_name,
                   int issue_number, const char *branch_name, char *response) {
  char prompt[MAX_BUFFER_SIZE];
  snprintf(prompt, sizeof(prompt), REVIEW_PROMPT_TEMPLATE, repo_owner,
           repo_name, branch_name);

  if (send_ai_request(prompt, response) != 0) {
    log_message(issue_number, "Failed to send AI request for review.");
    return -1;
  }

  log_message(issue_number, "Received AI response for review.");
  return 0;
}

int final_review(const char *repo_owner, const char *repo_name,
                 int issue_number, const char *branch_name, char *response) {
  char prompt[MAX_BUFFER_SIZE];
  snprintf(prompt, sizeof(prompt), FINAL_REVIEW_PROMPT_TEMPLATE, repo_owner,
           repo_name, branch_name);

  if (send_ai_request(prompt, response) != 0) {
    log_message(issue_number, "Failed to send AI request for final review.");
    return -1;
  }

  log_message(issue_number, "Received AI response for final review.");
  return 0;
}

int create_pr(const char *repo_owner, const char *repo_name, int issue_number,
              const char *branch_name, char *response) {
  char pr_title[256];
  char pr_body[1024];
  // Extract pr_title and pr_body from the AI response
  // This is a placeholder, you should implement proper parsing of the AI
  // response
  sscanf(response, "Title: %255[^\n]\nBody: %1023[^\n]", pr_title, pr_body);
  char prompt[MAX_BUFFER_SIZE];
  snprintf(prompt, sizeof(prompt), PR_PROMPT_TEMPLATE, repo_owner, repo_name,
           branch_name);

  if (send_ai_request(prompt, response) != 0) {
    log_message(issue_number, "Failed to send AI request for PR creation.");
    return -1;
  }

  log_message(issue_number, "Received AI response for PR creation.");
  return 0;
}

// Mock function to simulate cloning a repository
int mock_clone_repository(const char *repo_owner, const char *repo_name,
                          const char *local_path, int issue_number) {
  log_message(issue_number, "Mocking: Cloned repository %s/%s to %s",
              repo_owner, repo_name, local_path);
  return 0;
}

// Mock function to simulate creating and checking out a branch
int mock_create_and_checkout_branch(const char *branch_name,
                                    const char *local_path, int issue_number) {
  log_message(issue_number, "Mocking: Created and checked out branch %s in %s",
              branch_name, local_path);
  return 0;
}

// Mock function to simulate applying code changes
int mock_apply_code_changes(const char *local_path, const char *ai_response,
                            int issue_number) {
  (void)ai_response; // Suppress unused parameter warning
  log_message(issue_number,
              "Mocking: Applied code changes based on AI response in %s",
              local_path);
  return 0;
}

// Mock function to simulate committing and pushing changes
int mock_commit_and_push_changes(const char *local_path,
                                 const char *branch_name,
                                 const char *commit_message, int issue_number) {
  (void)local_path; // Suppress unused parameter warning
  log_message(
      issue_number,
      "Mocking: Committed and pushed changes to branch %s with message: %s",
      branch_name, commit_message);
  return 0;
}

// Mock function to simulate creating a pull request
int mock_create_pull_request(const char *repo_owner, const char *repo_name,
                             int issue_number, const char *branch_name,
                             const char *pr_title, const char *pr_body) {
  (void)pr_title; // Suppress unused parameter warning
  (void)pr_body;  // Suppress unused parameter warning
  log_message(issue_number,
              "Mocking: Created pull request for %s/%s from branch %s",
              repo_owner, repo_name, branch_name);
  return 0;
}

// Main processing function
int process_issue(const char *repo_owner, const char *repo_name,
                  int issue_number, const char *issue_title,
                  const char *issue_body) {
  char response[MAX_BUFFER_SIZE];
  char local_repo_path[256];
  char branch_name[64];

  syslog(LOG_INFO, "Processing issue #%d for %s/%s", issue_number, repo_owner,
         repo_name);

  snprintf(local_repo_path, sizeof(local_repo_path), "/tmp/%s_%s_%d",
           repo_owner, repo_name, issue_number);
  snprintf(branch_name, sizeof(branch_name), "issue_%d_fix", issue_number);

  // Mock: Clone the repository
  if (mock_clone_repository(repo_owner, repo_name, local_repo_path,
                            issue_number) != 0) {
    log_message(issue_number, "Failed to mock clone repository.");
    return -1;
  }

  // Mock: Create and checkout a new branch
  if (mock_create_and_checkout_branch(branch_name, local_repo_path,
                                      issue_number) != 0) {
    log_message(issue_number, "Failed to mock create and checkout branch.");
    return -1;
  }

  // Step 1: Analyze issue
  if (analyze_issue(repo_owner, repo_name, issue_number, issue_body,
                    response) != 0) {
    log_message(issue_number, "Failed to analyze issue.");
    return -1;
  }
  log_message(issue_number, "Issue Analysis Response: %s", response);

  // Step 2: Implement changes
  if (implement_issue(repo_owner, repo_name, issue_number, branch_name,
                      response) != 0) {
    log_message(issue_number, "Failed to implement changes.");
    return -1;
  }
  log_message(issue_number, "Implementation Response: %s", response);

  // Mock: Apply code changes based on AI response
  if (mock_apply_code_changes(local_repo_path, response, issue_number) != 0) {
    log_message(issue_number, "Failed to mock apply code changes.");
    return -1;
  }

  // Step 3: Review changes
  if (review_changes(repo_owner, repo_name, issue_number, branch_name,
                     response) != 0) {
    log_message(issue_number, "Failed to review changes.");
    return -1;
  }
  log_message(issue_number, "Review Response: %s", response);

  // Step 4: Final review
  if (final_review(repo_owner, repo_name, issue_number, branch_name,
                   response) != 0) {
    log_message(issue_number, "Failed to perform final review.");
    return -1;
  }
  log_message(issue_number, "Final Review Response: %s", response);

  // Mock: Commit and push changes
  if (mock_commit_and_push_changes(local_repo_path, branch_name,
                                   "Automated fix for issue",
                                   issue_number) != 0) {
    log_message(issue_number, "Failed to mock commit and push changes.");
    return -1;
  }

  // Step 5: Create PR
  if (create_pr(repo_owner, repo_name, issue_number, branch_name, response) !=
      0) {
    log_message(issue_number, "Failed to create PR.");
    return -1;
  }
  log_message(issue_number, "PR Creation Response: %s", response);

  // Mock: Create PR via GitHub API
  if (mock_create_pull_request(repo_owner, repo_name, issue_number, branch_name,
                               issue_title,
                               "Automated PR for issue fix") != 0) {
    log_message(issue_number, "Failed to mock create pull request.");
    return -1;
  }

  // Clean up local repository
  char remove_cmd[512];
  snprintf(remove_cmd, sizeof(remove_cmd), "rm -rf %s", local_repo_path);
  if (system(remove_cmd) != 0) {
    log_message(issue_number, "Failed to clean up local repository.");
    return -1;
  }

  log_message(issue_number, "Successfully processed issue.");
  return 0;
}

// HTTP server callback
enum MHD_Result answer_to_connection(void *cls,
                                     struct MHD_Connection *connection,
                                     const char *url, const char *method,
                                     const char *version,
                                     const char *upload_data,
                                     size_t *upload_data_size, void **con_cls) {
  (void)cls;
  (void)url;
  (void)version;
  (void)con_cls;

  syslog(LOG_INFO, "Received new connection");

  static int aptr;
  if (0 != strcmp(method, "POST")) {
    syslog(LOG_INFO, "Rejected non-POST request");
    return MHD_NO; // We only support POST
  }

  if (&aptr != *con_cls) {
    // The first time only the headers are valid, do not respond in the first
    // call
    *con_cls = &aptr;
    return MHD_YES;
  }

  if (*upload_data_size != 0) {
    // Process the upload data (webhook payload)
    cJSON *json = cJSON_Parse(upload_data);
    if (!json) {
      syslog(LOG_ERR, "Failed to parse webhook payload");
      const char *response = "Invalid JSON";
      struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
          strlen(response), (void *)response, MHD_RESPMEM_PERSISTENT);
      MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, mhd_response);
      MHD_destroy_response(mhd_response);
      return MHD_YES;
    }

    // Extract necessary data from the webhook payload
    cJSON *issue = cJSON_GetObjectItem(json, "issue");
    cJSON *repository = cJSON_GetObjectItem(json, "repository");

    if (!issue || !repository) {
      syslog(LOG_ERR, "Invalid webhook payload");
      const char *response = "Invalid payload";
      struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
          strlen(response), (void *)response, MHD_RESPMEM_PERSISTENT);
      MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, mhd_response);
      MHD_destroy_response(mhd_response);
      cJSON_Delete(json);
      return MHD_YES;
    }

    cJSON *issue_number_item = cJSON_GetObjectItem(issue, "number");
    cJSON *issue_title_item = cJSON_GetObjectItem(issue, "title");
    cJSON *issue_body_item = cJSON_GetObjectItem(issue, "body");
    cJSON *repo_full_name_item = cJSON_GetObjectItem(repository, "full_name");

    if (!issue_number_item || !issue_title_item || !issue_body_item ||
        !repo_full_name_item) {
      syslog(LOG_ERR, "Incomplete webhook data");
      const char *response = "Incomplete data";
      struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
          strlen(response), (void *)response, MHD_RESPMEM_PERSISTENT);
      MHD_queue_response(connection, MHD_HTTP_BAD_REQUEST, mhd_response);
      MHD_destroy_response(mhd_response);
      cJSON_Delete(json);
      return MHD_YES;
    }

    // Prepare issue data to enqueue
    cJSON *issue_data_json = cJSON_CreateObject();
    cJSON_AddStringToObject(issue_data_json, "repository",
                            repo_full_name_item->valuestring);
    cJSON_AddNumberToObject(issue_data_json, "issue_number",
                            issue_number_item->valueint);
    cJSON_AddStringToObject(issue_data_json, "issue_title",
                            issue_title_item->valuestring);
    cJSON_AddStringToObject(issue_data_json, "issue_body",
                            issue_body_item->valuestring);

    char *issue_data = cJSON_PrintUnformatted(issue_data_json);

    // Enqueue issue data in Redis
    redisContext *redis_ctx = (redisContext *)cls;
    if (enqueue_issue(redis_ctx, issue_data) != 0) {
      syslog(LOG_ERR, "Failed to enqueue issue");
      const char *response = "Failed to enqueue issue";
      struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
          strlen(response), (void *)response, MHD_RESPMEM_PERSISTENT);
      MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                         mhd_response);
      MHD_destroy_response(mhd_response);
      cJSON_Delete(json);
      cJSON_free(issue_data);
      cJSON_Delete(issue_data_json);
      return MHD_YES;
    }

    cJSON_Delete(json);
    cJSON_free(issue_data);
    cJSON_Delete(issue_data_json);

    // Respond with 200 OK
    const char *response = "OK";
    struct MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(response), (void *)response, MHD_RESPMEM_PERSISTENT);
    MHD_queue_response(connection, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    *upload_data_size = 0;
    return MHD_YES;
  }

  // POST data fully received
  return MHD_YES;
}

// Global variables for graceful shutdown
volatile sig_atomic_t keep_running = 1;
volatile sig_atomic_t shutdown_initiated = 0;
struct MHD_Daemon *mhd_daemon = NULL;
redisContext *redis_ctx = NULL;

void signal_handler(int signum) {
  if (__atomic_test_and_set(&shutdown_initiated, __ATOMIC_SEQ_CST)) {
    syslog(LOG_INFO, "Shutdown already in progress, ignoring signal %d",
           signum);
    return;
  }

  syslog(LOG_INFO, "Received signal %d, shutting down...", signum);
  keep_running = 0;
  if (mhd_daemon) {
    syslog(LOG_INFO, "Stopping MHD daemon");
    MHD_stop_daemon(mhd_daemon);
    mhd_daemon = NULL;
  }
  if (redis_ctx) {
    syslog(LOG_INFO, "Closing Redis connection");
    redisFree(redis_ctx);
    redis_ctx = NULL;
  }
  syslog(LOG_INFO, "Shutdown complete");
}

// Function to simulate a webhook payload
char *simulate_webhook_payload() {
  cJSON *json = cJSON_CreateObject();
  cJSON *issue = cJSON_CreateObject();
  cJSON *repository = cJSON_CreateObject();

  cJSON_AddItemToObject(json, "issue", issue);
  cJSON_AddItemToObject(json, "repository", repository);

  cJSON_AddNumberToObject(issue, "number", 1);
  cJSON_AddStringToObject(issue, "title", "Test Issue");
  cJSON_AddStringToObject(issue, "body", "This is a test issue body.");
  cJSON_AddStringToObject(repository, "full_name", "test-owner/test-repo");

  char *payload = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  return payload;
}

// Function to run tests
void run_tests() {
  syslog(LOG_INFO, "Running tests...");
  // Add your test code here
  char *payload = simulate_webhook_payload();
  syslog(LOG_INFO, "Simulated webhook payload: %s", payload);

  // Process the simulated payload
  cJSON *json = cJSON_Parse(payload);
  if (json) {
    cJSON *issue = cJSON_GetObjectItem(json, "issue");
    cJSON *repository = cJSON_GetObjectItem(json, "repository");

    if (issue && repository) {
      cJSON *issue_number_item = cJSON_GetObjectItem(issue, "number");
      cJSON *issue_title_item = cJSON_GetObjectItem(issue, "title");
      cJSON *issue_body_item = cJSON_GetObjectItem(issue, "body");
      cJSON *repo_full_name_item = cJSON_GetObjectItem(repository, "full_name");

      if (issue_number_item && issue_title_item && issue_body_item &&
          repo_full_name_item) {
        cJSON *issue_data_json = cJSON_CreateObject();
        cJSON_AddStringToObject(issue_data_json, "repository",
                                repo_full_name_item->valuestring);
        cJSON_AddNumberToObject(issue_data_json, "issue_number",
                                issue_number_item->valueint);
        cJSON_AddStringToObject(issue_data_json, "issue_title",
                                issue_title_item->valuestring);
        cJSON_AddStringToObject(issue_data_json, "issue_body",
                                issue_body_item->valuestring);

        char *issue_data = cJSON_PrintUnformatted(issue_data_json);
        if (issue_data == NULL) {
          syslog(LOG_ERR, "Failed to print JSON data");
        } else {
          if (enqueue_issue(redis_ctx, issue_data) == 0) {
            syslog(LOG_INFO, "Successfully enqueued simulated issue");
          } else {
            syslog(LOG_ERR, "Failed to enqueue simulated issue");
          }
          cJSON_free(issue_data);
        }
        cJSON_Delete(issue_data_json);
      }
    }
    cJSON_Delete(json);
  }
  free(payload);

  syslog(LOG_INFO, "Tests completed.");
}

// Main function
int main(int argc, char *argv[]) {
  // Set up signal handler
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  configure_logging();
  syslog(LOG_INFO, "Starting code_issue_service");

  int opt;
  int test_mode = 0;
  char *config_file = NULL;

  while ((opt = getopt(argc, argv, "tc:")) != -1) {
    switch (opt) {
    case 't':
      test_mode = 1;
      break;
    case 'c':
      config_file = optarg;
      break;
    default:
      fprintf(stderr, "Usage: %s [-t] -c <config_file_path>\n", argv[0]);
      return 1;
    }
  }

  if (!config_file) {
    fprintf(stderr, "Usage: %s [-t] -c <config_file_path>\n", argv[0]);
    return 1;
  }

  // Load configuration
  if (ini_parse(config_file, config_handler, NULL) < 0) {
    syslog(LOG_ERR, "Cannot load config file: %s", config_file);
    return 1;
  }

  // Initialize Redis
  redis_ctx = redisConnect(REDIS_HOST, REDIS_PORT);
  if (redis_ctx == NULL || redis_ctx->err) {
    if (redis_ctx) {
      syslog(LOG_ERR, "Failed to connect to Redis: %s", redis_ctx->errstr);
      redisFree(redis_ctx);
    } else {
      syslog(LOG_ERR, "Failed to allocate Redis context");
    }
    return 1;
  }

  // Start the worker thread
  pthread_t worker_thread;
  if (pthread_create(&worker_thread, NULL, process_issue_thread, redis_ctx) !=
      0) {
    syslog(LOG_ERR, "Failed to create worker thread");
    return 1;
  }

  // Simulate webhook payload
  char *payload = simulate_webhook_payload();
  syslog(LOG_INFO, "Simulated webhook payload: %s", payload);

  // Process the simulated payload
  cJSON *json = cJSON_Parse(payload);
  if (json) {
    cJSON *issue = cJSON_GetObjectItem(json, "issue");
    cJSON *repository = cJSON_GetObjectItem(json, "repository");

    if (issue && repository) {
      cJSON *issue_number_item = cJSON_GetObjectItem(issue, "number");
      cJSON *issue_title_item = cJSON_GetObjectItem(issue, "title");
      cJSON *issue_body_item = cJSON_GetObjectItem(issue, "body");
      cJSON *repo_full_name_item = cJSON_GetObjectItem(repository, "full_name");

      if (issue_number_item && issue_title_item && issue_body_item &&
          repo_full_name_item) {
        cJSON *issue_data_json = cJSON_CreateObject();
        cJSON_AddStringToObject(issue_data_json, "repository",
                                repo_full_name_item->valuestring);
        cJSON_AddNumberToObject(issue_data_json, "issue_number",
                                issue_number_item->valueint);
        cJSON_AddStringToObject(issue_data_json, "issue_title",
                                issue_title_item->valuestring);
        cJSON_AddStringToObject(issue_data_json, "issue_body",
                                issue_body_item->valuestring);

        char *issue_data = cJSON_PrintUnformatted(issue_data_json);

        if (enqueue_issue(redis_ctx, issue_data) == 0) {
          syslog(LOG_INFO, "Successfully enqueued simulated issue");
        } else {
          syslog(LOG_ERR, "Failed to enqueue simulated issue");
        }

        cJSON_free(issue_data);
        cJSON_Delete(issue_data_json);
      }
    }
    cJSON_Delete(json);
  }
  free(payload);

  // Ensure log directory exists
  struct stat st = {0};
  if (stat(LOG_DIRECTORY, &st) == -1) {
    mkdir(LOG_DIRECTORY, 0700);
  }

  // Initialize Redis
  redis_ctx = redisConnect(REDIS_HOST, REDIS_PORT);
  if (redis_ctx == NULL || redis_ctx->err) {
    if (redis_ctx) {
      syslog(LOG_ERR, "Failed to connect to Redis: %s", redis_ctx->errstr);
      redisFree(redis_ctx);
    } else {
      syslog(LOG_ERR, "Failed to allocate Redis context");
    }
    return 1;
  }

  if (test_mode) {
    run_tests();
  } else {
    // Start the worker thread
    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL, process_issue_thread, redis_ctx) !=
        0) {
      syslog(LOG_ERR, "Failed to create worker thread");
      return 1;
    }

    // Start the HTTP server
    mhd_daemon =
        MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, SERVER_PORT, NULL, NULL,
                         &answer_to_connection, redis_ctx, MHD_OPTION_END);
    if (mhd_daemon == NULL) {
      syslog(LOG_ERR, "Failed to start server");
      return 1;
    }

    syslog(LOG_INFO, "Server started on port %d", SERVER_PORT);

    // Keep the main thread running
    while (keep_running) {
      sleep(1);
    }

    // Clean up
    pthread_cancel(worker_thread);
    pthread_join(worker_thread, NULL);
  }

  if (redis_ctx) {
    redisFree(redis_ctx);
  }

  syslog(LOG_INFO, "Server shutting down");
  closelog();

  return 0;
}
