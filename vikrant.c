#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>

#define BUFFER_SIZE 65507  // Max UDP packet size
#define DEFAULT_PORT 8080
#define DEFAULT_THREADS 8
#define DEFAULT_DURATION 60  // Default 60 seconds
#define IMAGE_SIZE (2 * 1024 * 1024)  // 2 MB
#define TARGET_GB 12  // Target 12 GB
#define STATS_INTERVAL 2  // Update stats every 2 seconds

typedef struct {
    int sockfd;
    struct sockaddr_in dest_addr;
    char *image_data;
    int thread_id;
    volatile int *running;
    pthread_mutex_t *stats_mutex;
    unsigned long long *thread_bytes;
    unsigned long long *total_bytes;
    int packet_size;
} thread_args_t;

// Global variables
volatile int g_running = 1;
unsigned long long g_total_bytes = 0;
unsigned long long g_total_packets = 0;
pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
struct timeval g_start_time;
unsigned long long g_target_bytes = TARGET_GB * 1024ULL * 1024ULL * 1024ULL;

void signal_handler(int sig) {
    printf("\n\n[!] Interrupt received. Stopping threads...\n");
    g_running = 0;
}

void* sender_thread(void *arg) {
    thread_args_t *args = (thread_args_t*)arg;
    char *buffer = malloc(args->packet_size);
    if (!buffer) {
        printf("[Thread %d] Failed to allocate buffer\n", args->thread_id);
        return NULL;
    }
    
    // Fill buffer with image data pattern
    int image_pos = 0;
    unsigned long long local_bytes = 0;
    unsigned long long local_packets = 0;
    
    while (*args->running) {
        // Copy image data to buffer (cycling through 2MB image)
        int bytes_to_copy = args->packet_size;
        int remaining = IMAGE_SIZE - image_pos;
        
        if (bytes_to_copy <= remaining) {
            memcpy(buffer, args->image_data + image_pos, bytes_to_copy);
            image_pos += bytes_to_copy;
        } else {
            memcpy(buffer, args->image_data + image_pos, remaining);
            memcpy(buffer + remaining, args->image_data, bytes_to_copy - remaining);
            image_pos = bytes_to_copy - remaining;
        }
        
        if (image_pos >= IMAGE_SIZE) {
            image_pos = 0;
        }
        
        // Send packet
        ssize_t sent = sendto(args->sockfd, buffer, args->packet_size, 0,
                              (struct sockaddr*)&args->dest_addr, sizeof(args->dest_addr));
        
        if (sent > 0) {
            pthread_mutex_lock(args->stats_mutex);
            *args->total_bytes += sent;
            *args->thread_bytes += sent;
            g_total_packets++;
            pthread_mutex_unlock(args->stats_mutex);
            
            local_bytes += sent;
            local_packets++;
        } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Error, but continue
            if (local_packets % 10000 == 0) {
                printf("[Thread %d] Warning: %s\n", args->thread_id, strerror(errno));
            }
        }
        
        // Small yield to prevent CPU saturation
        if (local_packets % 1000 == 0) {
            sched_yield();
        }
    }
    
    printf("[Thread %d] Stopped - Sent: %.2f GB (%.2f MB)\n", 
           args->thread_id,
           local_bytes / (1024.0 * 1024.0 * 1024.0),
           local_bytes / (1024.0 * 1024.0));
    
    free(buffer);
    return NULL;
}

void* stats_monitor(void *arg) {
    int *duration = (int*)arg;
    struct timeval current_time;
    unsigned long long last_bytes = 0;
    double last_time = 0;
    double peak_speed = 0;
    int seconds_elapsed = 0;
    
    while (g_running) {
        sleep(STATS_INTERVAL);
        
        gettimeofday(&current_time, NULL);
        double elapsed = (current_time.tv_sec - g_start_time.tv_sec) +
                         (current_time.tv_usec - g_start_time.tv_usec) / 1000000.0;
        
        pthread_mutex_lock(&g_stats_mutex);
        unsigned long long current_bytes = g_total_bytes;
        unsigned long long current_packets = g_total_packets;
        pthread_mutex_unlock(&g_stats_mutex);
        
        double bytes_diff = (double)(current_bytes - last_bytes);
        double time_diff = elapsed - last_time;
        
        double current_speed = 0;
        if (time_diff > 0) {
            current_speed = (bytes_diff / (1024.0 * 1024.0)) / time_diff;
            if (current_speed > peak_speed) peak_speed = current_speed;
        }
        
        double avg_speed = 0;
        if (elapsed > 0) {
            avg_speed = (current_bytes / (1024.0 * 1024.0)) / elapsed;
        }
        
        double gb_sent = current_bytes / (1024.0 * 1024.0 * 1024.0);
        double percentage = (current_bytes * 100.0) / g_target_bytes;
        int remaining_seconds = (*duration > 0) ? (*duration - (int)elapsed) : 0;
        
        // Clear line and print stats
        printf("\r\033[K");
        printf("╔════════════════════════════════════════════════════════════════════╗\n");
        printf("║                    CONTINUOUS TRAFFIC GENERATOR                    ║\n");
        printf("╠════════════════════════════════════════════════════════════════════╣\n");
        printf("║ Total Sent:     %10.2f GB / %d GB (%5.1f%%)                        ║\n", 
               gb_sent, TARGET_GB, percentage);
        printf("║ Packets Sent:   %15llu packets                                    ║\n", current_packets);
        printf("║ Time Elapsed:   %15.2f / %d seconds                               ║\n", elapsed, *duration);
        printf("║ Current Speed:  %15.2f MB/s                                       ║\n", current_speed);
        printf("║ Average Speed:  %15.2f MB/s                                       ║\n", avg_speed);
        printf("║ Peak Speed:     %15.2f MB/s                                       ║\n", peak_speed);
        
        if (remaining_seconds > 0) {
            printf("║ Time Remaining: %15d seconds                                    ║\n", remaining_seconds);
            // Estimate final size if continues at current speed
            double estimated_gb = gb_sent + (current_speed * remaining_seconds / 1024.0);
            printf("║ Estimated Final: %10.2f GB                                     ║\n", estimated_gb);
        }
        printf("╚════════════════════════════════════════════════════════════════════╝\n");
        
        // Move cursor up for next update (to overwrite previous stats)
        printf("\033[7A");  // Move up 7 lines
        
        last_bytes = current_bytes;
        last_time = elapsed;
        
        // Check if duration completed
        if (*duration > 0 && elapsed >= *duration) {
            printf("\n\n[✓] Duration completed! Stopping threads...\n");
            g_running = 0;
            break;
        }
    }
    
    return NULL;
}

char* load_image_data() {
    char *buffer = malloc(IMAGE_SIZE);
    if (!buffer) return NULL;
    
    FILE *image_file = fopen("image.jpg", "rb");
    if (image_file) {
        size_t read_size = fread(buffer, 1, IMAGE_SIZE, image_file);
        printf("[+] Loaded image file: image.jpg (%zu bytes)\n", read_size);
        fclose(image_file);
        
        // Pad if needed
        if (read_size < IMAGE_SIZE) {
            memset(buffer + read_size, 0xAA, IMAGE_SIZE - read_size);
            printf("[+] Padded image to 2MB\n");
        }
    } else {
        // Create dummy image data pattern
        printf("[!] No image.jpg found, creating dummy image data...\n");
        for (int i = 0; i < IMAGE_SIZE; i++) {
            buffer[i] = (i % 256);
        }
    }
    
    return buffer;
}

void print_usage(char *prog_name) {
    printf("\n╔════════════════════════════════════════════════════════════════════╗\n");
    printf("║         CONTINUOUS UDP TRAFFIC GENERATOR - VIKRANT                 ║\n");
    printf("╠════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Usage: %s <ip_address> [options]                                   ║\n", prog_name);
    printf("╠════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Required:                                                           ║\n");
    printf("║   ip_address          Destination IP address                        ║\n");
    printf("║                                                                     ║\n");
    printf("║ Options:                                                            ║\n");
    printf("║   -p, --port <port>   Destination port (default: 8080)             ║\n");
    printf("║   -t, --threads <num> Number of threads (1-20, default: 8)        ║\n");
    printf("║   -d, --duration <sec> Duration in seconds (default: 60)          ║\n");
    printf("║   -s, --size <gb>     Target size in GB (default: 12)              ║\n");
    printf("║   -h, --help          Show this help                                ║\n");
    printf("╠════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Examples:                                                           ║\n");
    printf("║   %s 192.168.1.100 -p 8080 -t 16 -d 120            ║\n", prog_name);
    printf("║   %s 10.0.0.1 -t 8 -d 300 -s 20                    ║\n", prog_name);
    printf("║   %s 172.16.0.5 -t 20 -d 60                        ║\n", prog_name);
    printf("╚════════════════════════════════════════════════════════════════════╝\n\n");
    exit(0);
}

int main(int argc, char *argv[]) {
    int sockfd;
    struct sockaddr_in dest_addr;
    pthread_t threads[20];
    pthread_t stats_thread;
    thread_args_t thread_args[20];
    unsigned long long thread_bytes[20];
    
    int num_threads = DEFAULT_THREADS;
    int port = DEFAULT_PORT;
    int duration = DEFAULT_DURATION;
    int target_gb = TARGET_GB;
    char *ip_address = NULL;
    char *image_data;
    
    // Parse arguments
    if (argc < 2) {
        print_usage(argv[0]);
    }
    
    ip_address = argv[1];
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i+1 < argc) port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (i+1 < argc) num_threads = atoi(argv[++i]);
            if (num_threads > 20) num_threads = 20;
            if (num_threads < 1) num_threads = 1;
        }
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) {
            if (i+1 < argc) duration = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--size") == 0) {
            if (i+1 < argc) target_gb = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
        }
    }
    
    // Update target bytes
    g_target_bytes = target_gb * 1024ULL * 1024ULL * 1024ULL;
    
    printf("\n╔════════════════════════════════════════════════════════════════════╗\n");
    printf("║         CONTINUOUS UDP TRAFFIC GENERATOR - VIKRANT                 ║\n");
    printf("╠════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Destination:      %s:%d                                            ║\n", ip_address, port);
    printf("║ Threads:          %d                                               ║\n", num_threads);
    printf("║ Duration:         %d seconds                                       ║\n", duration);
    printf("║ Target Size:      %d GB                                            ║\n", target_gb);
    printf("║ Image Size:       2 MB (cycling)                                   ║\n");
    printf("║ Packet Size:      %d bytes (max UDP)                               ║\n", BUFFER_SIZE);
    printf("║                                                                     ║\n");
    printf("║ [*] Sending will continue for %d seconds OR until Ctrl+C          ║\n", duration);
    printf("║ [*] Will exceed target if time permits                            ║\n");
    printf("╚════════════════════════════════════════════════════════════════════╝\n\n");
    
    // Load image data
    image_data = load_image_data();
    if (!image_data) {
        printf("[!] Failed to allocate image buffer\n");
        exit(EXIT_FAILURE);
    }
    
    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("[!] Socket creation failed");
        free(image_data);
        exit(EXIT_FAILURE);
    }
    
    // Optimize socket
    int opt = 1;
    int buffer_size = 256 * 1024 * 1024;  // 256 MB
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    
    // Configure destination
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip_address, &dest_addr.sin_addr) <= 0) {
        perror("[!] Invalid IP address");
        free(image_data);
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    
    // Set signal handler
    signal(SIGINT, signal_handler);
    
    // Start time
    gettimeofday(&g_start_time, NULL);
    
    // Create statistics monitor thread
    pthread_create(&stats_thread, NULL, stats_monitor, &duration);
    
    // Create sender threads
    printf("[+] Starting %d sender threads...\n\n", num_threads);
    sleep(1);
    
    for (int i = 0; i < num_threads; i++) {
        thread_bytes[i] = 0;
        thread_args[i].sockfd = sockfd;
        thread_args[i].dest_addr = dest_addr;
        thread_args[i].image_data = image_data;
        thread_args[i].thread_id = i;
        thread_args[i].running = &g_running;
        thread_args[i].stats_mutex = &g_stats_mutex;
        thread_args[i].thread_bytes = &thread_bytes[i];
        thread_args[i].total_bytes = &g_total_bytes;
        thread_args[i].packet_size = BUFFER_SIZE;
        
        if (pthread_create(&threads[i], NULL, sender_thread, &thread_args[i]) != 0) {
            printf("[!] Failed to create thread %d\n", i);
            g_running = 0;
            break;
        }
    }
    
    // Wait for duration to complete
    while (g_running) {
        sleep(1);
    }
    
    // Wait for all threads to complete
    printf("\n[+] Waiting for threads to finish...\n");
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(stats_thread, NULL);
    
    // Final statistics
    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    double total_time = (end_time.tv_sec - g_start_time.tv_sec) +
                        (end_time.tv_usec - g_start_time.tv_usec) / 1000000.0;
    
    double gb_sent = g_total_bytes / (1024.0 * 1024.0 * 1024.0);
    double avg_speed = (g_total_bytes / (1024.0 * 1024.0)) / total_time;
    
    printf("\n╔════════════════════════════════════════════════════════════════════╗\n");
    printf("║                   FINAL TRANSMISSION STATS                         ║\n");
    printf("╠════════════════════════════════════════════════════════════════════╣\n");
    printf("║ Total Data Sent:      %15.2f GB                                  ║\n", gb_sent);
    printf("║ Total Packets Sent:   %15llu packets                             ║\n", g_total_packets);
    printf("║ Total Time:           %15.2f seconds                             ║\n", total_time);
    printf("║ Average Speed:        %15.2f MB/s                                ║\n", avg_speed);
    printf("║ Target:               %d GB (%s)                                 ║\n", 
           target_gb, (gb_sent >= target_gb) ? "ACHIEVED ✓" : "NOT REACHED ✗");
    printf("╚════════════════════════════════════════════════════════════════════╝\n");
    
    // Cleanup
    free(image_data);
    close(sockfd);
    
    return 0;
}
