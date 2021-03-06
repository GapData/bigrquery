// [[Rcpp::depends(rapidjsonr)]]
#include <Rcpp.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include "rapidjson/filereadstream.h"
#include <RProgress.h>

#include <ctime>
#include <cstdio>
#include <stdlib.h>
#include <fstream>

#if defined(_WIN32) || defined(_WIN64)
#define timegm _mkgmtime
#endif

enum BqType {
  BQ_INTEGER,
  BQ_FLOAT,
  BQ_BOOLEAN,
  BQ_STRING,
  BQ_TIMESTAMP,
  BQ_TIME,
  BQ_DATE,
  BQ_DATETIME,
  BQ_RECORD
};

BqType parse_bq_type(std::string x) {
  if (x == "INTEGER") {
    return BQ_INTEGER;
  } else if (x == "FLOAT") {
    return BQ_FLOAT;
  } else if (x == "BOOLEAN") {
    return BQ_BOOLEAN;
  } else if (x == "STRING") {
    return BQ_STRING;
  } else if (x == "TIMESTAMP") {
    return BQ_TIMESTAMP;
  } else if (x == "TIME") {
    return BQ_TIME;
  } else if (x == "DATE") {
    return BQ_DATE;
  } else if (x == "DATETIME") {
    return BQ_DATETIME;
  } else if (x == "RECORD") {
    return BQ_RECORD;
  } else {
    Rcpp::stop("Unknown type %s", x);
  }
}

double parse_partial_seconds(char* string) {
  if (string == NULL || string[0] != '.')
    return 0;

  char *endptr;
  return strtod(string, &endptr);
}

class BqField {
private:
  std::string name_;
  BqType type_;
  bool array_;
  std::vector<BqField> fields_;

public:
  BqField(std::string name, BqType type, bool array = false) :
      name_(name), type_(type), array_(array)
  {
  }

  BqField(std::string name, std::vector<BqField> fields, bool array = false) :
      name_(name), type_(BQ_RECORD), array_(array), fields_(fields)
  {
  }

  BqField(const rapidjson::Value& field) {
    name_ = field["name"].GetString();
    array_ = std::string(field["mode"].GetString()) == "REPEATED";
    type_ = parse_bq_type(field["type"].GetString());

    if (field.HasMember("fields")) {
      const rapidjson::Value& fields = field["fields"];

      rapidjson::Value::ConstValueIterator field = fields.Begin(), end = fields.End();
      for ( ; field != end; ++field) {
        fields_.push_back(BqField(*field));
      }
    }
  }

  std::string name() const { return name_; }

  SEXP vectorInit(int n, bool array) const {
    if (array) {
      return Rcpp::List(n);
    }

    switch(type_) {
    case BQ_INTEGER:
      return Rcpp::IntegerVector(n);
    case BQ_FLOAT:
      return Rcpp::DoubleVector(n);
    case BQ_BOOLEAN:
      return Rcpp::LogicalVector(n);
    case BQ_STRING:
      return Rcpp::CharacterVector(n);
    case BQ_TIMESTAMP:
    case BQ_DATETIME:
      return Rcpp::DatetimeVector(n, "UTC");
    case BQ_DATE:
      return Rcpp::DateVector(n);
    case BQ_TIME:
      {
        Rcpp::DoubleVector out(n);
        out.attr("class") = Rcpp::CharacterVector::create("hms", "difftime");
        out.attr("units") = "secs";
        return out;
      }
    case BQ_RECORD:
      return Rcpp::List(n);
    }
  }

  SEXP vectorInit(int n) const  {
    return vectorInit(n, array_);
  }

  void vectorSet(SEXP x, int i, const rapidjson::Value& v, bool array) const {
    if (array && type_ != BQ_RECORD) {
      if (!v.IsArray())
        Rcpp::stop("Not an array [1]");

      int n = v.Size();
      SEXP out = vectorInit(n, false);
      for (int j = 0; j < n; ++j) {
        vectorSet(out, j, v[j]["v"], false);
      }

      SET_VECTOR_ELT(x, i, out);
      return;
    }

    switch(type_) {
    case BQ_INTEGER:
      INTEGER(x)[i] = v.IsString() ? atoi(v.GetString()) : NA_INTEGER;
      break;
    case BQ_TIMESTAMP:
    case BQ_FLOAT:
      REAL(x)[i] = v.IsString() ? atof(v.GetString()) : NA_REAL;
      break;
    case BQ_BOOLEAN:
      if (v.IsString()) {
        bool is_true = strncmp(v.GetString(), "T", 1) == 0;
        INTEGER(x)[i] = is_true;
      } else {
        INTEGER(x)[i] = NA_LOGICAL;
      }
      break;
    case BQ_STRING:
      if (v.IsString()) {
        SEXP chr = Rf_mkCharLenCE(v.GetString(), v.GetStringLength(), CE_UTF8);
        SET_STRING_ELT(x, i, chr);
      } else {
        SET_STRING_ELT(x, i, NA_STRING);
      }
      break;
    case BQ_TIME:
      if (v.IsString()) {
        struct tm dtm;
        char* parsed = strptime(v.GetString(), "%H:%M:%S", &dtm);

        if (parsed == NULL) {
          REAL(x)[i] = NA_REAL;
        } else {
          REAL(x)[i] = dtm.tm_hour * 3600 + dtm.tm_min * 60 + dtm.tm_sec +
            parse_partial_seconds(parsed);
        }
      } else {
        REAL(x)[i] = NA_REAL;
      }
      break;
    case BQ_DATE:
      if (v.IsString()) {
        Rcpp::Date date(v.GetString());
        REAL(x)[i] = date.getDate();
      } else {
        REAL(x)[i] = NA_REAL;
      }
      break;
    case BQ_DATETIME:
      if (v.IsString()) {
        struct tm dtm;
        char* parsed = strptime(v.GetString(), "%Y-%m-%dT%H:%M:%S", &dtm);

        if (parsed == NULL) {
          REAL(x)[i] = NA_REAL;
        } else {
          REAL(x)[i] = timegm(&dtm) + parse_partial_seconds(parsed);
        }
      } else {
        REAL(x)[i] = NA_REAL;
      }
      break;
    case BQ_RECORD:
      SET_VECTOR_ELT(x, i, recordValue(v));
      break;
    }
  }

  SEXP recordValue(const rapidjson::Value& v) const {
    int p = fields_.size();

    Rcpp::List out(p);
    Rcpp::CharacterVector names(p);
    out.attr("names") = names;

    if (!array_) {
      if (!v.IsObject())
        return out;

      const rapidjson::Value& f = v["f"];
      // f is array of fields
      if (!f.IsArray())
        Rcpp::stop("Not array [2]");

      for (int j = 0; j < p; ++j) {
        const BqField& field = fields_[j];
        const rapidjson::Value& vs = f[j]["v"];

        int n = (field.array_) ? vs.Size() : 1;
        SEXP col = field.vectorInit(n, false);
        if (field.array_) {
          for (int i = 0; i < n; ++i) {
            field.vectorSet(col, i, vs[i]["v"], false);
          }
        } else {
          field.vectorSet(col, 0, vs);
        }

        out[j] = col;
        names[j] = field.name_;
      }
    } else {
      int n = (v.IsArray()) ? v.Size() : 0;

      for (int j = 0; j < p; ++j) {
        const BqField& field = fields_[j];
        out[j] = field.vectorInit(n);
        names[j] = field.name_;
      }
      out.attr("class") = Rcpp::CharacterVector::create("tbl_df", "tbl", "data.frame");
      out.attr("row.names") = Rcpp::IntegerVector::create(NA_INTEGER, -n);

      if (n == 0)
        return out;

      for (int i = 0; i < n; ++i) {
        const rapidjson::Value& f = v[i]["v"]["f"];
        if (!f.IsArray())
          Rcpp::stop("Not an array [3]");

        for (int j = 0; j < p; ++j) {
          fields_[j].vectorSet(out[j], i, f[j]["v"]);
        }
      }
    }

    return out;
  }

  void vectorSet(SEXP x, int i, const rapidjson::Value& v) const  {
    vectorSet(x, i, v, array_);
  }

};

std::vector<BqField> bq_fields_parse(const rapidjson::Value& meta) {
  const rapidjson::Value& schema_fields = meta["schema"]["fields"];

  int p = schema_fields.Size();

  std::vector<BqField> fields;
  for (int j = 0; j < p; ++j) {
    fields.push_back(BqField(schema_fields[j]));
  }

  return fields;
}

Rcpp::List bq_fields_init(const std::vector<BqField>& fields, int n) {
  int p = fields.size();

  Rcpp::List out(p);
  Rcpp::CharacterVector names(p);
  for (int j = 0; j < p; ++j) {
    out[j] = fields[j].vectorInit(n);
    names[j] = fields[j].name();
  };
  out.attr("class") = Rcpp::CharacterVector::create("tbl_df", "tbl", "data.frame");
  out.attr("names") = names;
  out.attr("row.names") = Rcpp::IntegerVector::create(NA_INTEGER, -n);

  return out;
}

int bq_fields_set(const rapidjson::Value& data,
                  Rcpp::List out,
                  const std::vector<BqField>& fields,
                  int offset
                  ) {
  if (!data.HasMember("rows")) {
    // no rows
    return 0;
  }

  const rapidjson::Value& rows = data["rows"];
  int n = rows.Size(), p = fields.size();

  for (int i = 0; i < n; ++i) {
    const rapidjson::Value& f = rows[i]["f"];
    for (int j = 0; j < p; ++j) {
      fields[j].vectorSet(out[j], i + offset, f[j]["v"]);
    }
  }

  return n;
}

// [[Rcpp::export]]
SEXP bq_parse(std::string meta_s, std::string data_s) {
  rapidjson::Document meta_d;
  meta_d.Parse(&meta_s[0]);
  std::vector<BqField> fields = bq_fields_parse(meta_d);

  rapidjson::Document values_d;
  values_d.Parse(&data_s[0]);

  int n = (values_d.HasMember("rows")) ? values_d["rows"].Size() : 0;

  Rcpp::List out = bq_fields_init(fields, n);
  bq_fields_set(values_d, out, fields, 0);

  return out;
}

// [[Rcpp::export]]
SEXP bq_field_init(std::string json, std::string value = "") {
  rapidjson::Document d1;
  d1.Parse(&json[0]);

  BqField field(d1);
  SEXP out = field.vectorInit(1);

  if (value != "") {
    rapidjson::Document d2;
    d2.Parse(&value[0]);

    field.vectorSet(out, 0, d2);
  }

  return out;
}

// [[Rcpp::export]]
SEXP bq_parse_files(std::string schema_path,
                    std::vector<std::string> file_paths,
                    int n,
                    bool quiet) {

  // Generate field specification
  rapidjson::Document schema_doc;
  std::ifstream schema_stream(schema_path.c_str());
  rapidjson::IStreamWrapper schema_stream_w(schema_stream);
  schema_doc.ParseStream(schema_stream_w);

  std::vector<BqField> fields = bq_fields_parse(schema_doc);
  Rcpp::List out = bq_fields_init(fields, n);

  std::vector<std::string>::const_iterator it = file_paths.begin(),
    it_end = file_paths.end();

  RProgress::RProgress pb("Parsing [:bar] ETA: :eta");
  pb.set_total(file_paths.size());

  int offset = 0;
  char readBuffer[100 * 1024];

  for ( ; it != it_end; ++it) {
    FILE* values_file = fopen(it->c_str(), "rb");
    rapidjson::FileReadStream values_stream(values_file, readBuffer, sizeof(readBuffer));
    rapidjson::Document values_doc;
    values_doc.ParseStream(values_stream);

    if (values_doc.HasParseError()) {
      Rcpp::stop("Failed to parse '%s'", *it);
      fclose(values_file);
    }

    offset += bq_fields_set(values_doc, out, fields, offset);
    if (!quiet)
      pb.tick();

    fclose(values_file);
  }

  return out;
}
