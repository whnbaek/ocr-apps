#include "XSbench_header.h"
#include "ocr.h"
// OCR timer library
#include "timer.h"

// Prints program logo
void logo(int version)
{
    border_print();
    ocrPrintf(
    "                   __   __ ___________                 _                        \n"
    "                   \\ \\ / //  ___| ___ \\               | |                       \n"
    "                    \\ V / \\ `--.| |_/ / ___ _ __   ___| |__                     \n"
    "                    /   \\  `--. \\ ___ \\/ _ \\ '_ \\ / __| '_ \\                    \n"
    "                   / /^\\ \\/\\__/ / |_/ /  __/ | | | (__| | | |                   \n"
    "                   \\/   \\/\\____/\\____/ \\___|_| |_|\\___|_| |_|                   \n\n"
    );
    border_print();
    center_print("Ported to OCR at Intel", 79);
    center_print("Version: 1.0", 79); //Hard-coded
    border_print();
}

// Prints Section titles in center of 80 char terminal
void center_print(const char *s, int width)
{
    int length = strlen(s);
    int i;
    for (i=0; i<=(width-length)/2; i++) {
        ocrPrintf(" ");

    }
    ocrPrintf("%s", s);
    ocrPrintf("\n");
}

void print_results( Inputs in, double runtime )
{
    // Calculate Lookups per sec
    int lookups_per_sec = (int) ((double) in.lookups / runtime);

    border_print();
    center_print("RESULTS", 79);
    border_print();

    ocrPrintf("Runtime:     %.3f seconds\n", runtime);
    ocrPrintf("Lookups:     "); fancy_int(in.lookups);
    ocrPrintf("Lookups/s:   "); fancy_int(lookups_per_sec);
    print_throughput_custom("Lookups", (unsigned long long) in.lookups, runtime, (double)lookups_per_sec);
    border_print();
}

void print_inputs(Inputs in, int version )
{
    // Calculate Estimate of Memory Usage
    int mem_tot = estimate_mem_usage( in );
    logo(version);
    center_print("INPUT SUMMARY", 79);
    border_print();
    ocrPrintf("Materials:                    %d\n", in.n_mats);
    ocrPrintf("H-M Benchmark Size:           %s\n", in.HM);
    ocrPrintf("Total Nuclides:               %ld\n", in.n_isotopes);
    ocrPrintf("Gridpoints (per Nuclide):     ");
    fancy_int(in.n_gridpoints);
    ocrPrintf("Unionized Energy Gridpoints:  ");
    fancy_int(in.n_isotopes*in.n_gridpoints);
    ocrPrintf("XS Lookups:                   "); fancy_int(in.lookups);
    ocrPrintf("Places:                       %d\n", in.places);
    ocrPrintf("Lookups in flight per place:  %d\n", in.batch);
    ocrPrintf("Est. Memory Usage (MB):       "); fancy_int(mem_tot);
    border_print();
    center_print("INITIALIZATION", 79);
    border_print();
}

void border_print(void)
{
    ocrPrintf(
    "==================================================================="
    "=============\n");
}

// Prints comma separated integers - for ease of reading
void fancy_int( long a )
{
    ocrPrintf("%ld\n",a);
}

void print_CLI_error(void)
{
    ocrPrintf("Usage: ./XSBench <options>\n");
    ocrPrintf("Options include:\n");
    ocrPrintf("  -s <size>        Size of H-M Benchmark to run (small, large, XL, XXL)\n");
    ocrPrintf("  -g <gridpoints>  Number of gridpoints per nuclide (overrides -s defaults)\n");
    ocrPrintf("  -l <lookups>     Number of Cross-section (XS) lookups\n");
    ocrPrintf("  -p <places>      Partitions of the lookup range, one replica each (default 1)\n");
    ocrPrintf("  -b <batch>       Lookup chains in flight per place (default 1024)\n");
    ocrPrintf("Default is equivalent to: -s small -l 15000 -p 1 -b 1024\n");
    ocrPrintf("See readme for full description of default run values\n");
    ocrShutdown();
}

Inputs read_CLI( int argc, char * argv[] )
{
    Inputs input;

    input.batch = 1024;

    input.places = 1;

    // defaults to 355 (corresponding to H-M Large benchmark)
    input.n_isotopes = 355;

    input.n_mats = 12;

    // defaults to 11303 (corresponding to H-M Large benchmark)
    input.n_gridpoints = 11303;

    input.lookups = 15000;

    strcpy( input.HM, "small" );

    // Check if user sets these
    int user_g = 0;

    // Collect Raw Input
    for( int i = 1; i < argc; i++ )
    {
        char * arg = ocrGetArgv(argv, i);

        // Sync-batch width (-b)
        if( strcmp(arg, "-b") == 0 )
        {
            if( ++i < argc )
                input.batch = atoi(ocrGetArgv(argv,i));
            else
                print_CLI_error();
        }
        // places (-p)
        else if( strcmp(arg, "-p") == 0 )
        {
            if( ++i < argc )
                input.places = atoi(ocrGetArgv(argv,i));
            else
                print_CLI_error();
        }
        // n_gridpoints (-g)
        else if( strcmp(arg, "-g") == 0 )
        {
            if( ++i < argc )
            {
                user_g = 1;
                input.n_gridpoints = atoi(ocrGetArgv(argv, i));
            }
            else
                print_CLI_error();
        }
        // lookups (-l)
        else if( strcmp(arg, "-l") == 0 )
        {
            if( ++i < argc )
                input.lookups = atoi(ocrGetArgv(argv,i));
            else
                print_CLI_error();
        }
        // HM (-s)
        else if( strcmp(arg, "-s") == 0 )
        {
            if( ++i < argc )
                strcpy(input.HM, ocrGetArgv(argv,i));
            else
                print_CLI_error();
        }
        else {
            print_CLI_error();
        }
    }

    // Validate Input

    if( input.batch < 1 )
        print_CLI_error();

    if( input.places < 1 )
        print_CLI_error();

    // Validate n_isotopes
    if( input.n_isotopes < 1 )
        print_CLI_error();

    // Validate n_gridpoints
    if( input.n_gridpoints < 1 )
        print_CLI_error();

    // Validate lookups
    if( input.lookups < 1 )
        print_CLI_error();

    // A place with an empty share of the lookup range would build a replica it
    // never reads.
    if( input.places > input.lookups )
    {
        ocrPrintf("ERROR: -p <places> must not exceed -l <lookups>\n");
        print_CLI_error();
    }

    // Validate HM size
    if( strcasecmp(input.HM, "small") != 0 &&
        strcasecmp(input.HM, "large") != 0 &&
        strcasecmp(input.HM, "XL") != 0 &&
        strcasecmp(input.HM, "XXL") != 0 )
        print_CLI_error();

    // Set HM size specific parameters
    // (defaults to large)
    if( strcasecmp(input.HM, "small") == 0 )
        input.n_isotopes = 68;
    else if( strcasecmp(input.HM, "XL") == 0 && user_g == 0 )
        input.n_gridpoints = 238847; // sized to make 120 GB XS data
    else if( strcasecmp(input.HM, "XXL") == 0 && user_g == 0 )
        input.n_gridpoints = 238847 * 2.1; // 252 GB XS data

    // Return input struct
    return input;
}
