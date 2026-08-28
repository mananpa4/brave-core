/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_ads/core/internal/targeting/contextual/text_classification/resource/text_classification_probabilities_database_table.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "brave/components/brave_ads/core/internal/common/database/database_column_util.h"
#include "brave/components/brave_ads/core/internal/common/database/database_table_util.h"
#include "brave/components/brave_ads/core/internal/common/database/database_transaction_util.h"
#include "brave/components/brave_ads/core/internal/common/logging_util.h"
#include "brave/components/brave_ads/core/mojom/brave_ads.mojom.h"

namespace brave_ads::database::table {

namespace {

// `kVisitsTableName` holds one row per page visit and is the "one" side of
// the relationship. `kTableName` holds one row per segment score for that
// visit and is the "many" side, referencing its parent via `visit_id`.
constexpr char kVisitsTableName[] = "text_classification_probability_visits";
constexpr char kTableName[] = "text_classification_probabilities";

void BindVisitColumnTypes(const mojom::DBActionInfoPtr& mojom_db_action) {
  CHECK(mojom_db_action);

  mojom_db_action->bind_column_types = {
      mojom::DBBindColumnType::kTime  // created_at
  };
}

void BindProbabilityColumnTypes(const mojom::DBActionInfoPtr& mojom_db_action) {
  CHECK(mojom_db_action);

  mojom_db_action->bind_column_types = {
      mojom::DBBindColumnType::kString,  // segment
      mojom::DBBindColumnType::kDouble   // page_score
  };
}

size_t BindProbabilityColumns(const mojom::DBActionInfoPtr& mojom_db_action,
                              const TextClassificationProbabilityMap&
                                  probabilities) {
  CHECK(mojom_db_action);
  CHECK(!probabilities.empty());

  size_t row_count = 0;

  int32_t index = 0;
  for (const auto& [segment, page_score] : probabilities) {
    BindColumnString(mojom_db_action, index++, segment);
    BindColumnDouble(mojom_db_action, index++, page_score);

    ++row_count;
  }

  return row_count;
}

void GetAllCallback(
    GetTextClassificationProbabilitiesCallback callback,
    mojom::DBTransactionResultInfoPtr mojom_db_transaction_result) {
  if (!IsTransactionSuccessful(mojom_db_transaction_result)) {
    BLOG(0, "Failed to get text classification probabilities");
    return std::move(callback).Run(
        /*success=*/false, /*text_classification_probabilities=*/{});
  }

  CHECK(mojom_db_transaction_result->rows_union);

  // Rows are grouped by `visit_id`, newest visit first, with each group of
  // rows reconstructing one page visit's probability map.
  TextClassificationProbabilityList text_classification_probabilities;
  std::optional<int64_t> last_visit_id;
  for (const auto& mojom_db_row :
       mojom_db_transaction_result->rows_union->get_rows()) {
    const int64_t visit_id = ColumnInt64(mojom_db_row, 0);
    const std::string segment = ColumnString(mojom_db_row, 1);
    const double page_score = ColumnDouble(mojom_db_row, 2);

    if (!last_visit_id || visit_id != *last_visit_id) {
      text_classification_probabilities.emplace_back();
      last_visit_id = visit_id;
    }

    text_classification_probabilities.back().insert({segment, page_score});
  }

  std::move(callback).Run(/*success=*/true,
                          std::move(text_classification_probabilities));
}

std::string BuildInsertVisitSql(const mojom::DBActionInfoPtr& mojom_db_action,
                                base::Time created_at) {
  CHECK(mojom_db_action);

  BindColumnTime(mojom_db_action, /*index=*/0, created_at);

  return base::ReplaceStringPlaceholders(
      R"(
          INSERT INTO $1 (
            created_at
          ) VALUES (?))",
      {kVisitsTableName}, nullptr);
}

std::string BuildInsertProbabilitiesSql(
    const mojom::DBActionInfoPtr& mojom_db_action,
    const TextClassificationProbabilityMap& probabilities) {
  CHECK(mojom_db_action);
  CHECK(!probabilities.empty());

  const size_t row_count =
      BindProbabilityColumns(mojom_db_action, probabilities);

  // `last_insert_rowid()` is connection-wide and updates after every row this
  // statement inserts, so it cannot be reused across multiple `VALUES` rows
  // in the same statement without each row after the first picking up the
  // previous row's own newly-generated id instead of the visit's id. Scoping
  // the lookup to `$2` avoids that, since this statement never inserts into
  // `$2` itself, so its most recent `id` stays stable for every row here.
  const std::string visit_id_subquery = base::ReplaceStringPlaceholders(
      "(SELECT MAX(id) FROM $1)", {kVisitsTableName}, nullptr);
  const std::vector<std::string> row_placeholders(
      row_count,
      base::ReplaceStringPlaceholders("($1, ?, ?)", {visit_id_subquery},
                                      nullptr));

  return base::ReplaceStringPlaceholders(
      R"(
          INSERT INTO $1 (
            visit_id,
            segment,
            page_score
          ) VALUES $2)",
      {kTableName, base::JoinString(row_placeholders, ", ")}, nullptr);
}

void InsertVisit(const mojom::DBTransactionInfoPtr& mojom_db_transaction,
                 base::Time created_at) {
  CHECK(mojom_db_transaction);

  mojom::DBActionInfoPtr mojom_db_action = mojom::DBActionInfo::New();
  mojom_db_action->type = mojom::DBActionInfo::Type::kExecuteWithBindings;
  mojom_db_action->sql = BuildInsertVisitSql(mojom_db_action, created_at);
  BindVisitColumnTypes(mojom_db_action);
  mojom_db_transaction->actions.push_back(std::move(mojom_db_action));
}

void InsertProbabilities(
    const mojom::DBTransactionInfoPtr& mojom_db_transaction,
    const TextClassificationProbabilityMap& probabilities) {
  CHECK(mojom_db_transaction);

  if (probabilities.empty()) {
    return;
  }

  mojom::DBActionInfoPtr mojom_db_action = mojom::DBActionInfo::New();
  mojom_db_action->type = mojom::DBActionInfo::Type::kExecuteWithBindings;
  mojom_db_action->sql =
      BuildInsertProbabilitiesSql(mojom_db_action, probabilities);
  BindProbabilityColumnTypes(mojom_db_action);
  mojom_db_transaction->actions.push_back(std::move(mojom_db_action));
}

}  // namespace

void TextClassificationProbabilities::Save(
    const TextClassificationProbabilityMap& probabilities,
    base::Time created_at,
    ResultCallback callback) {
  if (probabilities.empty()) {
    return std::move(callback).Run(/*success=*/true);
  }

  mojom::DBTransactionInfoPtr mojom_db_transaction =
      mojom::DBTransactionInfo::New();

  InsertVisit(mojom_db_transaction, created_at);
  InsertProbabilities(mojom_db_transaction, probabilities);

  RunTransaction(FROM_HERE, std::move(mojom_db_transaction),
                 std::move(callback));
}

void TextClassificationProbabilities::SaveAll(
    const TextClassificationProbabilityList& probabilities_history,
    base::Time newest_created_at,
    ResultCallback callback) {
  if (probabilities_history.empty()) {
    return std::move(callback).Run(/*success=*/true);
  }

  mojom::DBTransactionInfoPtr mojom_db_transaction =
      mojom::DBTransactionInfo::New();

  // Assign each visit a strictly decreasing synthetic timestamp so the
  // original newest-first ordering can be reconstructed from `created_at`.
  // All visit/probability action pairs are added to the same transaction so
  // they commit together atomically instead of as separate transactions.
  int64_t index = 0;
  for (const auto& probabilities : probabilities_history) {
    if (probabilities.empty()) {
      continue;
    }

    InsertVisit(mojom_db_transaction,
               newest_created_at - base::Milliseconds(index++));
    InsertProbabilities(mojom_db_transaction, probabilities);
  }

  RunTransaction(FROM_HERE, std::move(mojom_db_transaction),
                 std::move(callback));
}

void TextClassificationProbabilities::PruneToMaximumEntries(
    size_t maximum_entries,
    ResultCallback callback) {
  mojom::DBTransactionInfoPtr mojom_db_transaction =
      mojom::DBTransactionInfo::New();

  // Keep the `maximum_entries` most recent visits and their probabilities,
  // and delete the rest.
  Execute(mojom_db_transaction, R"(
      DELETE FROM
        $1
      WHERE
        visit_id NOT IN (
          SELECT
            id
          FROM
            $2
          ORDER BY
            created_at DESC
          LIMIT $3
        ))",
          {kTableName, kVisitsTableName, base::NumberToString(maximum_entries)});

  Execute(mojom_db_transaction, R"(
      DELETE FROM
        $1
      WHERE
        id NOT IN (
          SELECT
            id
          FROM
            $1
          ORDER BY
            created_at DESC
          LIMIT $2
        ))",
          {kVisitsTableName, base::NumberToString(maximum_entries)});

  RunTransaction(FROM_HERE, std::move(mojom_db_transaction),
                 std::move(callback));
}

void TextClassificationProbabilities::DeleteAll(ResultCallback callback) {
  mojom::DBTransactionInfoPtr mojom_db_transaction =
      mojom::DBTransactionInfo::New();
  Execute(mojom_db_transaction, R"(
      DELETE FROM
        $1)",
          {kTableName});
  Execute(mojom_db_transaction, R"(
      DELETE FROM
        $1)",
          {kVisitsTableName});

  RunTransaction(FROM_HERE, std::move(mojom_db_transaction),
                 std::move(callback));
}

void TextClassificationProbabilities::GetAll(
    GetTextClassificationProbabilitiesCallback callback) const {
  mojom::DBTransactionInfoPtr mojom_db_transaction =
      mojom::DBTransactionInfo::New();
  mojom::DBActionInfoPtr mojom_db_action = mojom::DBActionInfo::New();
  mojom_db_action->type = mojom::DBActionInfo::Type::kExecuteQueryWithBindings;
  mojom_db_action->sql = base::ReplaceStringPlaceholders(
      R"(
          SELECT
            v.id,
            p.segment,
            p.page_score
          FROM
            $1 AS p
            INNER JOIN $2 AS v
              ON v.id = p.visit_id
          ORDER BY
            v.created_at DESC,
            v.id DESC)",
      {kTableName, kVisitsTableName}, nullptr);
  mojom_db_action->bind_column_types = {
      mojom::DBBindColumnType::kInt64,  // v.id
      mojom::DBBindColumnType::kString,  // p.segment
      mojom::DBBindColumnType::kDouble   // p.page_score
  };
  mojom_db_transaction->actions.push_back(std::move(mojom_db_action));

  RunTransaction(FROM_HERE, std::move(mojom_db_transaction),
                 base::BindOnce(&GetAllCallback, std::move(callback)));
}

void TextClassificationProbabilities::Create(
    const mojom::DBTransactionInfoPtr& mojom_db_transaction) {
  CHECK(mojom_db_transaction);

  Execute(mojom_db_transaction, R"(
      CREATE TABLE text_classification_probability_visits (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        created_at TIMESTAMP NOT NULL
      ))");

  CreateTableIndex(mojom_db_transaction, kVisitsTableName,
                   /*columns=*/{"created_at"});

  Execute(mojom_db_transaction, R"(
      CREATE TABLE text_classification_probabilities (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        visit_id INTEGER NOT NULL REFERENCES
            text_classification_probability_visits (id),
        segment TEXT NOT NULL,
        page_score REAL NOT NULL
      ))");

  CreateTableIndex(mojom_db_transaction, kTableName,
                   /*columns=*/{"visit_id"});
}

void TextClassificationProbabilities::Migrate(
    const mojom::DBTransactionInfoPtr& mojom_db_transaction,
    int to_version) {
  CHECK(mojom_db_transaction);

  switch (to_version) {
    case 59: {
      Create(mojom_db_transaction);
      break;
    }

    default: {
      // No migration needed.
      break;
    }
  }
}

}  // namespace brave_ads::database::table
