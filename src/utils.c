#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void get_config_from_yaml(const char *filename, char *ip_buf, size_t max_ip_len, char *port_buf, size_t max_port_len)
{
	FILE *file = fopen(filename, "r");
	if (!file) {
		perror("Failed to open YAML config");
		exit(EXIT_FAILURE);
	}

	char line[256];
	int ip_found = 0, port_found = 0;
	while (fgets(line, sizeof(line), file)) {
		char *key = strtok(line, ": \t\n");
		char *val = strtok(NULL, ": \t\n\"");
		if (key && val) {
			if (strcmp(key, "ip") == 0) {
				strncpy(ip_buf, val, max_ip_len);
				ip_found = 1;
			} else if (strcmp(key, "port") == 0) {
				strncpy(port_buf, val, max_port_len);
				port_found = 1;
			}
		}
	}
	fclose(file);

	if (!ip_found || !port_found) {
		fprintf(stderr, "Could not find 'ip' and/or 'port' in %s\n", filename);
		exit(EXIT_FAILURE);
	}
}
