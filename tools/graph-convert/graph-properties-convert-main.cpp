#include "graph-properties-convert.h"

#include "galois/Galois.h"
#include "galois/Timer.h"
#include "galois/ErrorCode.h"
#include "galois/Logging.h"
#include "Transforms.h"

#include <llvm/Support/CommandLine.h>

namespace cll = llvm::cl;

namespace {

cll::opt<std::string> input_filename(cll::Positional,
                                     cll::desc("<input file/directory>"),
                                     cll::Required);
cll::opt<std::string>
    output_directory(cll::Positional,
                     cll::desc("<local ouput directory/s3 directory>"),
                     cll::Required);
cll::opt<galois::SourceType>
    type(cll::desc("Input file type:"),
         cll::values(clEnumValN(galois::SourceType::kGraphml, "graphml",
                                "source file is of type GraphML"),
                     clEnumValN(galois::SourceType::kKatana, "katana",
                                "source file is of type Katana")),
         cll::init(galois::SourceType::kGraphml));
cll::opt<galois::SourceDatabase>
    database(cll::desc("Database the data was exported from:"),
             cll::values(clEnumValN(galois::SourceDatabase::kNeo4j, "neo4j",
                                    "source data came from Neo4j"),
                         clEnumValN(galois::SourceDatabase::kMongodb, "mongodb",
                                    "source data came from mongodb")),
             cll::init(galois::SourceDatabase::kNone));
cll::opt<int>
    chunk_size("chunkSize",
               cll::desc("Chunk size for in memory arrow representation during "
                         "converions, generally this term can be ignored, but "
                         "it can be decreased for improving memory usage when "
                         "converting large inputs"),
               cll::init(25000));

cll::list<std::string> timestamp_properties("timestamp",
                                            cll::desc("Timestamp properties"));

galois::graphs::PropertyFileGraph ConvertKatana(const std::string& rdg_file) {
  auto result = galois::graphs::PropertyFileGraph::Make(rdg_file);
  if (!result) {
    GALOIS_LOG_FATAL("failed to load {}: {}", rdg_file, result.error());
  }

  std::unique_ptr<galois::graphs::PropertyFileGraph> graph =
      std::move(result.value());

  std::vector<std::string> t_fields;
  std::copy(timestamp_properties.begin(), timestamp_properties.end(),
            std::back_insert_iterator<std::vector<std::string>>(t_fields));

  std::vector<std::unique_ptr<galois::ColumnTransformer>> transformers;
  transformers.emplace_back(std::make_unique<galois::SparsifyBooleans>());
  if (!t_fields.empty()) {
    transformers.emplace_back(
        std::make_unique<galois::ConvertTimestamps>(t_fields));
  }

  ApplyTransforms(graph.get(), transformers);

  return galois::graphs::PropertyFileGraph(std::move(*graph));
}

void ParseWild() {
  switch (type) {
  case galois::SourceType::kGraphml:
    return galois::WritePropertyGraph(
        galois::ConvertGraphML(input_filename, chunk_size), output_directory);
  case galois::SourceType::kKatana:
    return galois::WritePropertyGraph(ConvertKatana(input_filename),
                                      output_directory);
  default:
    GALOIS_LOG_ERROR("Unsupported input type {}", type);
  }
}

void ParseNeo4j() {
  galois::GraphComponents graph;
  switch (type) {
  case galois::SourceType::kGraphml:
    return galois::WritePropertyGraph(
        galois::ConvertGraphML(input_filename, chunk_size), output_directory);
  default:
    GALOIS_LOG_ERROR("Unsupported input type {}", type);
  }
}

void ParseMongoDB() {
  GALOIS_LOG_WARN("MongoDB importing is under development");
}

} // namespace

int main(int argc, char** argv) {
  galois::SharedMemSys sys;
  llvm::cl::ParseCommandLineOptions(argc, argv);

  galois::StatTimer total_timer("TimerTotal");
  total_timer.start();
  if (chunk_size <= 0) {
    chunk_size = 25000;
  }

  switch (database) {
  case galois::SourceDatabase::kNone:
    ParseWild();
    break;
  case galois::SourceDatabase::kNeo4j:
    ParseNeo4j();
    break;
  case galois::SourceDatabase::kMongodb:
    ParseMongoDB();
    break;
  }

  total_timer.stop();
}
