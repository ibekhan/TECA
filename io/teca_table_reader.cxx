#include "teca_table_reader.h"
#include "teca_table.h"
#include "teca_coordinate_util.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <errno.h>

using std::string;
using std::endl;
using std::cerr;

#if defined(TECA_HAS_BOOST)
#include <boost/program_options.hpp>
#endif

#if defined(TECA_HAS_MPI)
#include <mpi.h>
#endif

// PIMPL idiom
struct teca_table_reader::teca_table_reader_internals
{
    teca_table_reader_internals()
        : number_of_steps(0) {}

    void clear();

    static p_teca_table read_table(
        const std::string &file_name, bool distribute);

    p_teca_table table;
    teca_metadata metadata;
    unsigned long number_of_steps;
    std::vector<unsigned long> step_counts;
    std::vector<unsigned long> step_offsets;
    std::vector<unsigned long> step_ids;
};

// --------------------------------------------------------------------------
void teca_table_reader::teca_table_reader_internals::clear()
{
    this->table = nullptr;
    this->number_of_steps = 0;
    this->metadata.clear();
    this->step_counts.clear();
    this->step_offsets.clear();
    this->step_ids.clear();
}

// --------------------------------------------------------------------------
p_teca_table
teca_table_reader::teca_table_reader_internals::read_table(
    const std::string &file_name, bool distribute)
{
    teca_binary_stream bs;

#if defined(TECA_HAS_MPI)
    int init = 0;
    int rank = 0;
    MPI_Initialized(&init);
    if (init)
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // rank 0 will read the data, must be rank 0 for the
    // case where using as a serial reader, but running in
    // parallel. rank 0 is assumed to have the data in the
    // serial table based algorithms.
    const int root_rank = 0;
    if (rank == root_rank)
    {
#endif
        // open the file
        FILE* fd = fopen(file_name.c_str(), "rb");
        if (fd == NULL)
        {
            const char *estr = strerror(errno);
            TECA_ERROR("Failed to open " << file_name << ". " << estr)
            return nullptr;
        }

        // get its length, we'll read it in one go and need to create
        // a bufffer for it's contents
        long start = ftell(fd);
        fseek(fd, 0, SEEK_END);
        long end = ftell(fd);
        fseek(fd, 0, SEEK_SET);
        long nbytes = end - start - 10;

        // check if this is really ours
        char id[11] = {'\0'};
        if (fread(id, 1, 10, fd) != 10)
        {
            const char *estr = (ferror(fd) ? strerror(errno) : "");
            fclose(fd);
            TECA_ERROR("Failed to read \"" << file_name << "\". " << estr)
            return nullptr;
        }

        if (strncmp(id, "teca_table", 10))
        {
            fclose(fd);
            TECA_ERROR("Not a teca_table. \"" << file_name << "\"")
            return nullptr;
        }

        // create the buffer
        bs.resize(static_cast<size_t>(nbytes));

        // read the stream
        long bytes_read = fread(bs.get_data(), sizeof(unsigned char), nbytes, fd);
        if (bytes_read != nbytes)
        {
            const char *estr = (ferror(fd) ? strerror(errno) : "");
            fclose(fd);
            TECA_ERROR("Failed to read \"" << file_name << "\". Read only "
                << bytes_read << " of the requested " << nbytes << ". " << estr)
            return nullptr;
        }
        fclose(fd);
#if defined(TECA_HAS_MPI)
        if (init && distribute)
        {
            MPI_Bcast(&nbytes, 1, MPI_LONG, root_rank, MPI_COMM_WORLD);
            MPI_Bcast(bs.get_data(), nbytes, MPI_BYTE, root_rank, MPI_COMM_WORLD);
        }
    }
    else
    if (init && distribute)
    {
        long nbytes = 0;
        MPI_Bcast(&nbytes, 1, MPI_LONG, root_rank, MPI_COMM_WORLD);
        bs.resize(static_cast<size_t>(nbytes));
        MPI_Bcast(bs.get_data(), nbytes, MPI_BYTE, root_rank, MPI_COMM_WORLD);
    }
    else
    {
        return nullptr;
    }
#endif
    // deserialize the binary rep
    p_teca_table table = teca_table::New();
    table->from_stream(bs);
    return table;
}


// --------------------------------------------------------------------------
teca_table_reader::teca_table_reader() : generate_original_ids(0)
{
    this->internals = new teca_table_reader_internals;
}

// --------------------------------------------------------------------------
teca_table_reader::~teca_table_reader()
{
    delete this->internals;
}

#if defined(TECA_HAS_BOOST)
// --------------------------------------------------------------------------
void teca_table_reader::get_properties_description(
    const string &prefix, options_description &global_opts)
{
    options_description opts("Options for "
        + (prefix.empty()?"teca_table_reader":prefix));

    opts.add_options()
        TECA_POPTS_GET(string, prefix, file_name,
            "a file name to read")
        TECA_POPTS_GET(string, prefix, index_column,
            "name of the column containing index values (\"\")")
        TECA_POPTS_GET(int, prefix, generate_original_ids,
            "add original row ids into the output. default off.")
        ;

    global_opts.add(opts);
}

// --------------------------------------------------------------------------
void teca_table_reader::set_properties(const string &prefix, variables_map &opts)
{
    TECA_POPTS_SET(opts, string, prefix, file_name)
    TECA_POPTS_SET(opts, string, prefix, index_column)
    TECA_POPTS_SET(opts, int, prefix, generate_original_ids)
}
#endif

// --------------------------------------------------------------------------
void teca_table_reader::clear_cached_metadata()
{
    this->internals->clear();
}

// --------------------------------------------------------------------------
teca_metadata teca_table_reader::get_output_metadata(unsigned int port,
    const std::vector<teca_metadata> &input_md)
{
#ifdef TECA_DEBUG
    cerr << teca_parallel_id()
        << "teca_cf_reader::get_output_metadata" << endl;
#endif
    (void)port;
    (void)input_md;

    // if result is cached use that
    if (this->internals->table)
        return this->internals->metadata;

    // read the data
    bool distribute = !this->index_column.empty();

    this->internals->table =
        teca_table_reader::teca_table_reader_internals::read_table(
            this->file_name, distribute);

    // when no index column is specified  act like a serial reader
    if (!this->internals->table || !distribute)
        return teca_metadata();

    // build the data structures for random access
    p_teca_variant_array index =
        this->internals->table->get_column(this->index_column);

    if (!index)
    {
        this->clear_cached_metadata();
        TECA_ERROR("Table is missing the index array \""
            << this->index_column << "\"")
        return teca_metadata();
    }

    TEMPLATE_DISPATCH_I(teca_variant_array_impl,
        index.get(),

        NT *pindex = dynamic_cast<TT*>(index.get())->get();

        teca_coordinate_util::get_table_offsets(pindex,
            this->internals->table->get_number_of_rows(),
            this->internals->number_of_steps, this->internals->step_counts,
            this->internals->step_offsets, this->internals->step_ids);
        )

    // must have at least one time step
    if (this->internals->number_of_steps < 1)
    {
        this->clear_cached_metadata();
        TECA_ERROR("Invalid index \"" << this->index_column << "\"")
        return teca_metadata();
    }

    // report about the number of steps, this is all that
    // is needed to run in parallel over time steps.
    teca_metadata md;
    md.insert("number_of_time_steps", this->internals->number_of_steps);

    // cache it
    this->internals->metadata = md;

    return md;
}


// --------------------------------------------------------------------------
const_p_teca_dataset teca_table_reader::execute(unsigned int port,
    const std::vector<const_p_teca_dataset> &input_data,
    const teca_metadata &request)
{
    (void)port;
    (void)input_data;

#if defined(TECA_HAS_MPI)
    int rank = 0;
    int init = 0;
    MPI_Initialized(&init);
    if (init)
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if ((rank == 0) && !this->internals->table)
    {
        TECA_ERROR("Failed to read data")
        return nullptr;
    }
#endif

    bool distribute = !this->index_column.empty();

    // not running in subsetting/parallel mode retrun
    // the complete table, it is empty off rank 0
    if (!distribute)
        return this->internals->table;

    // subset the table, pull out only rows for the requested step
    unsigned long step = 0;
    request.get("time_step", step);

    p_teca_table out_table = teca_table::New();
    out_table->copy_structure(this->internals->table);
    out_table->copy_metadata(this->internals->table);

    int ncols = out_table->get_number_of_columns();
    unsigned long nrows = this->internals->step_counts[step];
    unsigned long first_row = this->internals->step_offsets[step];

    for (int j = 0; j < ncols; ++j)
    {
        p_teca_variant_array in_col =
            this->internals->table->get_column(j);

        p_teca_variant_array out_col =
            out_table->get_column(j);

        out_col->resize(nrows);

        TEMPLATE_DISPATCH(teca_variant_array_impl,
            out_col.get(),
            NT *pin_col = static_cast<TT*>(in_col.get())->get();
            NT *pout_col = static_cast<TT*>(out_col.get())->get();
            memcpy(pout_col, pin_col+first_row, nrows*sizeof(NT));
            )
    }

    if (this->generate_original_ids)
    {
        p_teca_unsigned_long_array ids = teca_unsigned_long_array::New(nrows);
        unsigned long *pids = ids->get();
        for (unsigned long i = 0, q = first_row; i < nrows; ++i, ++q)
            pids[i] = q;
        out_table->append_column("original_ids", ids);
    }

    return out_table;
}
