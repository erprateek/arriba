#include <algorithm>
#include <ctime>
#include <iostream>
#include <set>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "common.hpp"
#include "annotation.hpp"
#include "assembly.hpp"
#include "options.hpp"
#include "read_stats.hpp"
#include "read_chimeric_alignments.hpp"
#include "filter_multi_mappers.hpp"
#include "filter_uninteresting_contigs.hpp"
#include "filter_inconsistently_clipped.hpp"
#include "filter_homopolymer.hpp"
#include "filter_duplicates.hpp"
#include "filter_proximal_read_through.hpp"
#include "filter_same_gene.hpp"
#include "filter_small_insert_size.hpp"
#include "filter_long_gap.hpp"
#include "filter_hairpin.hpp"
#include "filter_mismatches.hpp"
#include "filter_low_entropy.hpp"
#include "fusions.hpp"
#include "filter_relative_support.hpp"
#include "filter_both_intronic.hpp"
#include "filter_non_coding_neighbors.hpp"
#include "filter_intragenic_both_exonic.hpp"
#include "filter_min_support.hpp"
#include "recover_known_fusions.hpp"
#include "recover_both_spliced.hpp"
#include "filter_blacklisted_ranges.hpp"
#include "filter_pcr_fusions.hpp"
#include "merge_adjacent_fusions.hpp"
#include "select_best.hpp"
#include "filter_end_to_end.hpp"
#include "filter_short_anchor.hpp"
#include "filter_homologs.hpp"
#include "filter_mismappers.hpp"
#include "filter_no_coverage.hpp"
#include "filter_genomic_support.hpp"
#include "recover_many_spliced.hpp"
#include "recover_isoforms.hpp"
#include "output_fusions.hpp"

using namespace std;

unordered_map<string,filter_t> FILTERS({
	{"inconsistently_clipped", static_cast<string*>(NULL)},
	{"homopolymer", static_cast<string*>(NULL)},
	{"duplicates", static_cast<string*>(NULL)},
	{"read_through", static_cast<string*>(NULL)},
	{"same_gene", static_cast<string*>(NULL)},
	{"small_insert_size", static_cast<string*>(NULL)},
	{"long_gap", static_cast<string*>(NULL)},
	{"hairpin", static_cast<string*>(NULL)},
	{"mismatches", static_cast<string*>(NULL)},
	{"mismappers", static_cast<string*>(NULL)},
	{"relative_support", static_cast<string*>(NULL)},
	{"intronic", static_cast<string*>(NULL)},
	{"non_coding_neighbors", static_cast<string*>(NULL)},
	{"intragenic_exonic", static_cast<string*>(NULL)},
	{"min_support", static_cast<string*>(NULL)},
	{"known_fusions", static_cast<string*>(NULL)},
	{"spliced", static_cast<string*>(NULL)},
	{"blacklist", static_cast<string*>(NULL)},
	{"end_to_end", static_cast<string*>(NULL)},
	{"pcr_fusions", static_cast<string*>(NULL)},
	{"merge_adjacent", static_cast<string*>(NULL)},
	{"select_best", static_cast<string*>(NULL)},
	{"short_anchor", static_cast<string*>(NULL)},
	{"no_coverage", static_cast<string*>(NULL)},
	{"many_spliced", static_cast<string*>(NULL)},
	{"no_genomic_support", static_cast<string*>(NULL)},
	{"uninteresting_contigs", static_cast<string*>(NULL)},
	{"genomic_support", static_cast<string*>(NULL)},
	{"isoforms", static_cast<string*>(NULL)},
	{"low_entropy", static_cast<string*>(NULL)},
	{"homologs", static_cast<string*>(NULL)}
});

string get_time_string() {
	time_t now = time(0);
	char buffer[100];
	strftime(buffer, sizeof(buffer), "[%Y-%m-%dT%X]", localtime(&now));
	return buffer;
}

int main(int argc, char **argv) {

	// initialize filter names
	for (auto i = FILTERS.begin(); i != FILTERS.end(); ++i)
		i->second = &i->first; // filters are represented by pointers to the name of the filter (this saves memory compared to storing strings)

	// parse command-line options
	options_t options = parse_arguments(argc, argv);

	// convert options.interesting_contigs from string to contigs_t
	contigs_t interesting_contigs;
	if (options.filters.at("uninteresting_contigs") && !options.interesting_contigs.empty()) {
		istringstream iss(options.interesting_contigs);
		while (iss) {
			string contig;
			iss >> contig;
			if (!contig.empty())
				interesting_contigs.insert(pair<string,contig_t>(removeChr(contig),interesting_contigs.size()));
		}
	}
	contigs_t contigs = interesting_contigs;

	// load GTF file
	cout << get_time_string() << " Loading annotation from '" << options.gene_annotation_file << "'" << endl << flush;
	gene_annotation_t gene_annotation;
	transcript_annotation_t transcript_annotation;
	exon_annotation_t exon_annotation;
	unordered_map<string,gene_t> gene_names;
	read_annotation_gtf(options.gene_annotation_file, options.gtf_features, contigs, gene_annotation, transcript_annotation, exon_annotation, gene_names);

	// sort genes and exons by coordinate (make index)
	exon_annotation_index_t exon_annotation_index;
	make_annotation_index(exon_annotation, exon_annotation_index);
	gene_annotation_index_t gene_annotation_index;
	make_annotation_index(gene_annotation, gene_annotation_index);

	// load sequences of contigs from assembly
	cout << get_time_string() << " Loading assembly from '" << options.assembly_file << "'" << endl;
	assembly_t assembly;
	load_assembly(assembly, options.assembly_file, contigs, interesting_contigs);

	// prevent htslib from downloading the assembly via the Internet, if CRAM is used
	setenv("REF_PATH", ".", 0);

	// load chimeric alignments
	chimeric_alignments_t chimeric_alignments;
	unsigned long int mapped_reads = 0;
	coverage_t coverage(contigs, assembly);
	if (!options.chimeric_bam_file.empty()) { // when STAR was run with --chimOutType SeparateSAMold, chimeric alignments must be read from a separate file named Chimeric.out.sam
		cout << get_time_string() << " Reading chimeric alignments from '" << options.chimeric_bam_file << "'" << flush;
		cout << " (total=" << read_chimeric_alignments(options.chimeric_bam_file, options.assembly_file, chimeric_alignments, mapped_reads, coverage, contigs, interesting_contigs, gene_annotation_index, true, false) << ")" << endl;
	}

	// extract chimeric alignments and read-through alignments from Aligned.out.bam
	cout << get_time_string() << " Reading chimeric alignments from '" << options.rna_bam_file << "'" << flush;
	cout << " (total=" << read_chimeric_alignments(options.rna_bam_file, options.assembly_file, chimeric_alignments, mapped_reads, coverage, contigs, interesting_contigs, gene_annotation_index, !options.chimeric_bam_file.empty(), true) << ")" << endl;

	// map contig IDs to names
	vector<string> contigs_by_id(contigs.size());
	for (contigs_t::iterator i = contigs.begin(); i != contigs.end(); ++i)
		contigs_by_id[i->second] = i->first;

	// the BAM files may have added some contigs which were not in the GTF file
	// => add empty indices for the new contigs so that lookups of these contigs won't cause array-out-of-bounds exceptions
	gene_annotation_index.resize(contigs.size());
	exon_annotation_index.resize(contigs.size());

	cout << get_time_string() << " Filtering multi-mappers and single mates" << flush;
	cout << " (remaining=" << filter_multi_mappers(chimeric_alignments) << ")" << endl;

	strandedness_t strandedness = options.strandedness;
	if (options.strandedness == STRANDEDNESS_AUTO) {
		cout << get_time_string() << " Detecting strandedness" << flush;
		strandedness = detect_strandedness(chimeric_alignments, gene_annotation_index, exon_annotation_index);
		switch (strandedness) {
			case STRANDEDNESS_YES: cout << " (yes)" << endl; break;
			case STRANDEDNESS_REVERSE: cout << " (reverse)" << endl; break;
			default: cout << " (no)" << endl;
		}
	}
	if (strandedness != STRANDEDNESS_NO) {
		cout << get_time_string() << " Assigning strands to alignments" << endl << flush;
		assign_strands_from_strandedness(chimeric_alignments, strandedness);
	}

	cout << get_time_string() << " Annotating alignments" << flush << endl;
	// calculate sum of the lengths of all exons for each gene
	// we will need this to normalize the number of events over the gene length
	for (exon_annotation_index_t::iterator contig = exon_annotation_index.begin(); contig != exon_annotation_index.end(); ++contig) {
		position_t region_start = 0;
		for (exon_contig_annotation_index_t::iterator region = contig->begin(); region != contig->end(); ++region) {
			gene_t previous_gene = NULL;
			for (exon_set_t::iterator overlapping_exon = region->second.begin(); overlapping_exon != region->second.end(); ++overlapping_exon) {
				gene_t& current_gene = (**overlapping_exon).gene;
				if (previous_gene != current_gene) {
					current_gene->exonic_length += region->first - region_start;
					previous_gene = current_gene;
				}
			}
			region_start = region->first;
		}
	}
	for (gene_annotation_t::iterator gene = gene_annotation.begin(); gene != gene_annotation.end(); ++gene)
		if (gene->exonic_length == 0)
			gene->exonic_length = gene->end - gene->start; // use total gene length, if the gene has no exons

	// first, try to annotate with exons
	for (chimeric_alignments_t::iterator mates = chimeric_alignments.begin(); mates != chimeric_alignments.end(); ++mates)
		annotate_alignments(mates->second, exon_annotation_index);

	// if the alignment does not map to an exon, try to map it to a gene
	for (chimeric_alignments_t::iterator chimeric_alignment = chimeric_alignments.begin(); chimeric_alignment != chimeric_alignments.end(); ++chimeric_alignment) {
		for (mates_t::iterator mate = chimeric_alignment->second.begin(); mate != chimeric_alignment->second.end(); ++mate) {
			if (mate->genes.empty())
				get_annotation_by_coordinate(mate->contig, mate->start, mate->end, mate->genes, gene_annotation_index);
		}
		// try to resolve ambiguous mappings using mapping information from mate
		if (chimeric_alignment->second.size() == 3) {
			gene_set_t combined;
			combine_annotations(chimeric_alignment->second[SPLIT_READ].genes, chimeric_alignment->second[MATE1].genes, combined);
			if (chimeric_alignment->second[MATE1].genes.empty() || combined.size() < chimeric_alignment->second[MATE1].genes.size())
				chimeric_alignment->second[MATE1].genes = combined;
			if (chimeric_alignment->second[SPLIT_READ].genes.empty() || combined.size() < chimeric_alignment->second[SPLIT_READ].genes.size())
				chimeric_alignment->second[SPLIT_READ].genes = combined;
		}
	}

	// if the alignment maps neither to an exon nor to a gene, make a dummy gene which subsumes all alignments with a distance of 10kb
	gene_annotation_t unmapped_alignments;
	for (chimeric_alignments_t::iterator chimeric_alignment = chimeric_alignments.begin(); chimeric_alignment != chimeric_alignments.end(); ++chimeric_alignment) {
		gene_annotation_record_t gene_annotation_record;
		if (chimeric_alignment->second.size() == 3) { // split-read
			if (chimeric_alignment->second[SPLIT_READ].genes.empty()) {
				gene_annotation_record.contig = chimeric_alignment->second[SPLIT_READ].contig;
				gene_annotation_record.start = gene_annotation_record.end = (chimeric_alignment->second[SPLIT_READ].strand == FORWARD) ? chimeric_alignment->second[SPLIT_READ].start : chimeric_alignment->second[SPLIT_READ].end;
				unmapped_alignments.push_back(gene_annotation_record);
			}
			if (chimeric_alignment->second[SUPPLEMENTARY].genes.empty()) {
				gene_annotation_record.contig = chimeric_alignment->second[SUPPLEMENTARY].contig;
				gene_annotation_record.start = gene_annotation_record.end = (chimeric_alignment->second[SUPPLEMENTARY].strand == FORWARD) ? chimeric_alignment->second[SUPPLEMENTARY].end : chimeric_alignment->second[SUPPLEMENTARY].start;
				unmapped_alignments.push_back(gene_annotation_record);
			}
		} else { // discordant mates
			for (mates_t::iterator mate = chimeric_alignment->second.begin(); mate != chimeric_alignment->second.end(); ++mate) {
				if (mate->genes.empty()) {
					gene_annotation_record.contig = mate->contig;
					gene_annotation_record.start = gene_annotation_record.end = (mate->strand == FORWARD) ? mate->end : mate->start;
					unmapped_alignments.push_back(gene_annotation_record);
				}
			}
		}
	}
	if (unmapped_alignments.size() > 0) {
		unmapped_alignments.sort();
		gene_annotation_record_t gene_annotation_record;
		gene_annotation_record.contig = unmapped_alignments.begin()->contig;
		gene_annotation_record.start = unmapped_alignments.begin()->start;
		gene_annotation_record.end = unmapped_alignments.begin()->end;
		gene_annotation_record.strand = FORWARD;
		gene_annotation_record.exonic_length = 10000; //TODO more exact estimation of exonic_length
		gene_annotation_record.is_dummy = true;
		gene_annotation_record.is_protein_coding = false;
		gene_contig_annotation_index_t::iterator next_known_gene = gene_annotation_index[unmapped_alignments.begin()->contig].lower_bound(unmapped_alignments.begin()->end);
		for (gene_annotation_t::iterator unmapped_alignment = next(unmapped_alignments.begin()); ; ++unmapped_alignment) {
			// subsume all unmapped alignments in a range of 10kb into a dummy gene with the generic name "contig:start-end"
			if (unmapped_alignment == unmapped_alignments.end() || // all unmapped alignments have been processed => add last record
			    gene_annotation_record.end+10000 < unmapped_alignment->start || // current alignment is too far away
			    (next_known_gene != gene_annotation_index[gene_annotation_record.contig].end() && next_known_gene->first <= unmapped_alignment->start) || // dummy gene must not overlap known genes
			    unmapped_alignment->contig != gene_annotation_record.contig) { // end of contig reached
				gene_annotation_record.name = contigs_by_id[gene_annotation_record.contig] + ":" + to_string(static_cast<long long int>(gene_annotation_record.start)) + "-" + to_string(static_cast<long long int>(gene_annotation_record.end));
				gene_annotation.push_back(gene_annotation_record);
				if (unmapped_alignment != unmapped_alignments.end()) {
					gene_annotation_record.contig = unmapped_alignment->contig;
					gene_annotation_record.start = unmapped_alignment->start;
					next_known_gene = gene_annotation_index[unmapped_alignment->contig].lower_bound(unmapped_alignment->end);
				} else {
					break;
				}
			}
			gene_annotation_record.end = unmapped_alignment->end;
		}
	}

	// map yet unmapped alignments to the newly created dummy genes
	gene_annotation_index.clear();
	make_annotation_index(gene_annotation, gene_annotation_index); // index needs to be regenerated after adding dummy genes
	for (chimeric_alignments_t::iterator chimeric_alignment = chimeric_alignments.begin(); chimeric_alignment != chimeric_alignments.end(); ++chimeric_alignment) {
		for (mates_t::iterator mate = chimeric_alignment->second.begin(); mate != chimeric_alignment->second.end(); ++mate) {
			if (mate->genes.empty())
				get_annotation_by_coordinate(mate->contig, mate->start, mate->end, mate->genes, gene_annotation_index);
		}
		if (chimeric_alignment->second.size() == 3) // split-read
			if (chimeric_alignment->second[MATE1].genes.empty()) // copy dummy gene from split-read, if mate1 still has no annotation
				chimeric_alignment->second[MATE1].genes = chimeric_alignment->second[SPLIT_READ].genes;
	}

	// assign IDs to genes
	// this is necessary for deterministic behavior, because fusions are hashed by genes
	unsigned int gene_id = 0;
	for (gene_annotation_t::iterator gene = gene_annotation.begin(); gene != gene_annotation.end(); ++gene)
		gene->id = gene_id++;

	if (options.filters.at("duplicates")) {
		cout << get_time_string() << " Filtering duplicates" << flush;
		cout << " (remaining=" << filter_duplicates(chimeric_alignments) << ")" << endl;
	}

	if (options.filters.at("uninteresting_contigs") && !interesting_contigs.empty()) {
		cout << get_time_string() << " Filtering mates which do not map to interesting contigs (" << options.interesting_contigs << ")" << flush;
		cout << " (remaining=" << filter_uninteresting_contigs(chimeric_alignments, contigs, interesting_contigs) << ")" << endl;
	}

	cout << get_time_string() << " Estimating mate gap distribution" << flush;
	float mate_gap_mean, mate_gap_stddev;
	int max_mate_gap;
	if (estimate_mate_gap_distribution(chimeric_alignments, mate_gap_mean, mate_gap_stddev, gene_annotation_index, exon_annotation_index)) {
		cout << " (mean=" << mate_gap_mean << ", stddev=" << mate_gap_stddev << ")" << endl;
		max_mate_gap = max(0, (int) (mate_gap_mean + 3*mate_gap_stddev));
	} else
		max_mate_gap = options.fragment_length;
	
	if (options.filters.at("read_through")) {
		cout << get_time_string() << " Filtering read-through fragments with a distance <=" << options.min_read_through_distance << "bp" << flush;
		cout << " (remaining=" << filter_proximal_read_through(chimeric_alignments, options.min_read_through_distance) << ")" << endl;
	}

	if (options.filters.at("inconsistently_clipped")) {
		cout << get_time_string() << " Filtering inconsistently clipped mates" << flush;
		cout << " (remaining=" << filter_inconsistently_clipped_mates(chimeric_alignments) << ")" << endl;
	}

	if (options.filters.at("homopolymer")) {
		cout << get_time_string() << " Filtering breakpoints adjacent to homopolymers >=" << options.homopolymer_length << "nt" << flush;
		cout << " (remaining=" << filter_homopolymer(chimeric_alignments, options.homopolymer_length, exon_annotation_index) << ")" << endl;
	}

	if (options.filters.at("small_insert_size")) {
		cout << get_time_string() << " Filtering fragments with small insert size" << flush;
		cout << " (remaining=" << filter_small_insert_size(chimeric_alignments, 5) << ")" << endl;
	}

	if (options.filters.at("long_gap")) {
		cout << get_time_string() << " Filtering alignments with long gaps" << flush;
		cout << " (remaining=" << filter_long_gap(chimeric_alignments) << ")" << endl;
	}

	if (options.filters.at("same_gene")) {
		cout << get_time_string() << " Filtering fragments with both mates in the same gene" << flush;
		cout << " (remaining=" << filter_same_gene(chimeric_alignments, exon_annotation_index) << ")" << endl;
	}

	if (options.filters.at("hairpin")) {
		cout << get_time_string() << " Filtering fusions arising from hairpin structures" << flush;
		cout << " (remaining=" << filter_hairpin(chimeric_alignments, exon_annotation_index, max_mate_gap) << ")" << endl;
	}

	if (options.filters.at("mismatches")) {
		cout << get_time_string() << " Filtering reads with a mismatch p-value <=" << options.mismatch_pvalue_cutoff << flush;
		cout << " (remaining=" << filter_mismatches(chimeric_alignments, assembly, interesting_contigs, 0.01, options.mismatch_pvalue_cutoff) << ")" << endl;
	}

	if (options.filters.at("low_entropy")) {
		cout << get_time_string() << " Filtering reads with low entropy (k-mer content >=" << (options.max_kmer_content*100) << "%)" << flush;
		cout << " (remaining=" << filter_low_entropy(chimeric_alignments, 3, options.max_kmer_content) << ")" << endl;
	}

	cout << get_time_string() << " Finding fusions and counting supporting reads" << flush;
	fusions_t fusions;
	cout << " (total=" << find_fusions(chimeric_alignments, fusions, exon_annotation_index, max_mate_gap, options.subsampling_threshold) << ")" << endl;

	if (!options.genomic_breakpoints_file.empty()) {
		cout << get_time_string() << " Marking fusions with support from whole-genome sequencing in '" << options.genomic_breakpoints_file << "'" << flush;
		cout << " (marked=" << mark_genomic_support(fusions, options.genomic_breakpoints_file, contigs, options.max_genomic_breakpoint_distance) << ")" << endl;
	}

	if (options.filters.at("merge_adjacent")) {
		cout << get_time_string() << " Merging adjacent fusion breakpoints" << flush;
		cout << " (remaining=" << merge_adjacent_fusions(fusions, 5) << ")" << endl;
	}

	// this step must come after the 'merge_adjacent' filter,
	// because STAR clips reads supporting the same breakpoints at different position
	// and that spreads the supporting reads over multiple breakpoints
	cout << get_time_string() << " Estimating expected number of fusions by random chance (e-value)" << endl << flush;
	estimate_expected_fusions(fusions, mapped_reads, exon_annotation_index);

	// this step must come before all filters that are potentially undone by the 'genomic_support' filter
	if (options.filters.at("non_coding_neighbors")) {
		cout << get_time_string() << " Filtering fusions with both breakpoints in adjacent non-coding/intergenic regions" << flush;
		cout << " (remaining=" << filter_non_coding_neighbors(fusions) << ")" << endl;
	}

	// this step must come before all filters that are potentially undone by the 'genomic_support' filter
	if (options.filters.at("intragenic_exonic")) {
		cout << get_time_string() << " Filtering intragenic fusions with both breakpoints in exonic regions" << flush;
		cout << " (remaining=" << filter_intragenic_both_exonic(fusions, exon_annotation_index, options.exonic_fraction) << ")" << endl;
	}

	// this step must come after e-value calculation,
	// because fusions with few supporting reads heavily influence the e-value
	// it must come before all filters that are potentially undone by the 'genomic_support' filter
	if (options.filters.at("min_support")) {
		cout << get_time_string() << " Filtering fusions with <" << options.min_support << " supporting reads" << flush;
		cout << " (remaining=" << filter_min_support(fusions, options.min_support) << ")" << endl;
	}

	if (options.filters.at("relative_support")) {
		cout << get_time_string() << " Filtering fusions with an e-value >=" << options.evalue_cutoff << flush;
		cout << " (remaining=" << filter_relative_support(fusions, options.evalue_cutoff) << ")" << endl;
	}

	// this step must come before all filters that are potentially undone by the 'genomic_support' filter
	if (options.filters.at("intronic")) {
		cout << get_time_string() << " Filtering fusions with both breakpoints in intronic/intergenic regions" << flush;
		cout << " (remaining=" << filter_both_intronic(fusions) << ")" << endl;
	}

	// this step must come right after the 'relative_support' and 'min_support' filters
	if (!options.known_fusions_file.empty() && options.filters.at("known_fusions")) {
		cout << get_time_string() << " Searching for known fusions in '" << options.known_fusions_file << "'" << flush;
		cout << " (remaining=" << recover_known_fusions(fusions, options.known_fusions_file, gene_names, coverage) << ")" << endl;
	}

	// this step must come after the 'merge_adjacent' filter,
	// or else adjacent breakpoints will be counted several times
	// it must come before the 'spliced' and 'many_spliced' filters,
	// which are prone to recovering PCR-mediated fusions
	if (options.filters.at("pcr_fusions")) {
		cout << get_time_string() << " Filtering PCR/RT fusions between genes with an expression above the " << (options.high_expression_quantile*100) << "% quantile" << flush;
		cout << " (remaining=" << filter_pcr_fusions(fusions, chimeric_alignments, options.high_expression_quantile, gene_annotation_index) << ")" << endl;
	}

	// this step must come closely after the 'relative_support' and 'min_support' filters
	if (options.filters.at("spliced")) {
		cout << get_time_string() << " Searching for fusions with spliced split reads" << flush;
		cout << " (remaining=" << recover_both_spliced(fusions, 200) << ")" << endl;
	}

	// this step must come after the 'merge_adjacent' filter,
	// because merging might yield a different best breakpoint
	if (options.filters.at("select_best")) {
		cout << get_time_string() << " Selecting best breakpoints from genes with multiple breakpoints" << flush;
		cout << " (remaining=" << select_most_supported_breakpoints(fusions) << ")" << endl;
	}

	// this step must come after the 'select_best' filter, because it increases the chances of
	// an event to pass all filters by recovering multiple breakpoints which evidence the same event
	// moreover, this step must come after all the filters the 'relative_support' and 'min_support' filters
	if (options.filters.at("many_spliced")) {
		cout << get_time_string() << " Searching for fusions with >=" << options.min_spliced_events << " spliced events" << flush;
		cout << " (remaining=" << recover_many_spliced(fusions, options.min_spliced_events) << ")" << endl;
	}

	if (!options.genomic_breakpoints_file.empty() && options.filters.at("no_genomic_support")) {
		cout << get_time_string() << " Assigning confidence scores to events" << endl << flush;
		assign_confidence(fusions, coverage);

		// this step must come after assigning confidence scores
		cout << get_time_string() << " Filtering low-confidence events with no support from WGS" << flush;
		cout << " (remaining=" << filter_no_genomic_support(fusions) << ")" << endl;
	}

	// this step must come after the 'select_best' filter, because the 'select_best' filter prefers
	// soft-clipped breakpoints, which are easier to remove by blacklisting, because they are more recurrent
	if (options.filters.at("blacklist") && !options.blacklist_file.empty()) {
		cout << get_time_string() << " Filtering blacklisted fusions in '" << options.blacklist_file << "'" << flush;
		cout << " (remaining=" << filter_blacklisted_ranges(fusions, options.blacklist_file, contigs, gene_names, options.evalue_cutoff, max_mate_gap) << ")" << endl;
	}

	if (options.filters.at("short_anchor")) {
		cout << get_time_string() << " Filtering fusions with anchors <=" << options.min_anchor_length << "nt" << flush;
		cout << " (remaining=" << filter_short_anchor(fusions, options.min_anchor_length) << ")" << endl;
	}

	if (options.filters.at("end_to_end")) {
		cout << get_time_string() << " Filtering end-to-end fusions with low support" << flush;
		cout << " (remaining=" << filter_end_to_end_fusions(fusions) << ")" << endl;
	}

	if (options.filters.at("no_coverage")) {
		cout << get_time_string() << " Filtering fusions with no coverage around the breakpoints" << flush;
		cout << " (remaining=" << filter_no_coverage(fusions, coverage, exon_annotation_index, max_mate_gap) << ")" << endl;
	}

	// make kmer indices from gene sequences
	kmer_indices_t kmer_indices;
	const char kmer_length = 8; // must not be longer than 16 or else conversion to int will fail
	if (options.filters.at("homologs") || options.filters.at("mismappers")) {
		cout << get_time_string() << " Indexing gene sequences" << endl << flush;
		make_kmer_index(fusions, assembly, kmer_length, kmer_indices);
	}

	// this step must come near the end, because it is expensive in terms of memory consumption
	if (options.filters.at("homologs")) {
		cout << get_time_string() << " Filtering genes with >=" << (options.max_homolog_identity*100) << "% identity" << flush;
		cout << " (remaining=" << filter_homologs(fusions, kmer_indices, kmer_length, assembly, options.max_homolog_identity) << ")" << endl;
	}

	// this step must come near the end, because it is expensive in terms of memory and CPU consumption
	if (options.filters.at("mismappers")) {
		cout << get_time_string() << " Re-aligning chimeric reads to filter fusions with >=" << (options.max_mismapper_fraction*100) << "% mis-mappers" << flush;
		cout << " (remaining=" << filter_mismappers(fusions, kmer_indices, kmer_length, assembly, exon_annotation_index, options.max_mismapper_fraction, max_mate_gap) << ")" << endl;
	}

	// this step must come after all heuristic filters, to undo them
	if (!options.genomic_breakpoints_file.empty() && options.filters.at("genomic_support")) {
		cout << get_time_string() << " Searching for fusions with support from WGS" << flush;
		cout << " (remaining=" << recover_genomic_support(fusions) << ")" << endl;
	}

	if (!options.genomic_breakpoints_file.empty() && options.filters.at("genomic_support") || options.filters.at("many_spliced")) {
		// the 'select_best' filter needs to be run again, to remove redundant events recovered by the 'genomic_support' and 'many_spliced' filters
		if (options.filters.at("select_best")) {
			cout << get_time_string() << " Selecting best breakpoints from genes with multiple breakpoints" << flush;
			cout << " (remaining=" << select_most_supported_breakpoints(fusions) << ")" << endl;
		}
	}

	// this filter must come last, because it should only recover isoforms of fusions which pass all other filters
	if (options.filters.at("isoforms")) {
		cout << get_time_string() << " Searching for additional isoforms" << flush;
		cout << " (remaining=" << recover_isoforms(fusions) << ")" << endl;
	}

	// this step must come after the 'isoforms' filter, because recovered isoforms need to be scored anew
	cout << get_time_string() << " Assigning confidence scores to events" << endl << flush;
	assign_confidence(fusions, coverage);

	cout << get_time_string() << " Writing fusions to file '" << options.output_file << "'" << endl;
	write_fusions_to_file(fusions, options.output_file, coverage, assembly, gene_annotation_index, exon_annotation_index, contigs_by_id, options.print_supporting_reads, options.print_fusion_sequence, options.print_peptide_sequence, false);

	if (options.discarded_output_file != "") {
		cout << get_time_string() << " Writing discarded fusions to file '" << options.discarded_output_file << "'" << endl;
		write_fusions_to_file(fusions, options.discarded_output_file, coverage, assembly, gene_annotation_index, exon_annotation_index, contigs_by_id, options.print_supporting_reads_for_discarded_fusions, options.print_fusion_sequence_for_discarded_fusions, options.print_peptide_sequence_for_discarded_fusions, true);
	}

	return 0;
}
