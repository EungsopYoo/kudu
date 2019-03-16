// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/master/sentry_authz_provider.h"

#include <ostream>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include "kudu/common/table_util.h"
#include "kudu/gutil/macros.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/sentry/sentry_action.h"
#include "kudu/sentry/sentry_client.h"
#include "kudu/sentry/sentry_policy_service_types.h"
#include "kudu/thrift/client.h"
#include "kudu/util/flag_tags.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/slice.h"

using sentry::TListSentryPrivilegesRequest;
using sentry::TListSentryPrivilegesResponse;
using sentry::TSentryAuthorizable;
using sentry::TSentryGrantOption;
using std::string;
using std::vector;

DEFINE_string(sentry_service_rpc_addresses, "",
              "Comma-separated list of RPC addresses of the Sentry service(s). When "
              "set, Sentry integration is enabled, fine-grained access control is "
              "enforced in the master, and clients are issued authorization tokens. "
              "Must match the value of the sentry.service.client.server.rpc-addresses "
              "option in the Sentry server configuration.");
TAG_FLAG(sentry_service_rpc_addresses, experimental);

DEFINE_string(server_name, "server1",
              "Configures which server namespace the Kudu instance belongs to for defining "
              "server-level privileges in Sentry. Used to distinguish a particular Kudu "
              "cluster in case of a multi-cluster setup. Must match the value of the "
              "hive.sentry.server option in the HiveServer2 configuration, and the value "
              "of the --server_name in Impala configuration.");
TAG_FLAG(server_name, experimental);

DEFINE_string(kudu_service_name, "kudu",
              "The service name of the Kudu server. Must match the service name "
              "used for Kudu server of sentry.service.admin.group option in the "
              "Sentry server configuration.");
TAG_FLAG(kudu_service_name, experimental);

DEFINE_string(sentry_service_kerberos_principal, "sentry",
              "The service principal of the Sentry server. Must match the primary "
              "(user) portion of sentry.service.server.principal option in the "
              "Sentry server configuration.");
TAG_FLAG(sentry_service_kerberos_principal, experimental);

DEFINE_string(sentry_service_security_mode, "kerberos",
              "Configures whether Thrift connections to the Sentry server use "
              "SASL (Kerberos) security. Must match the value of the "
              "‘sentry.service.security.mode’ option in the Sentry server "
              "configuration.");
TAG_FLAG(sentry_service_security_mode, experimental);

DEFINE_int32(sentry_service_retry_count, 1,
             "The number of times that Sentry operations will retry after "
             "encountering retriable failures, such as network errors.");
TAG_FLAG(sentry_service_retry_count, advanced);
TAG_FLAG(sentry_service_retry_count, experimental);

DEFINE_int32(sentry_service_send_timeout_seconds, 60,
             "Configures the socket send timeout, in seconds, for Thrift "
             "connections to the Sentry server.");
TAG_FLAG(sentry_service_send_timeout_seconds, advanced);
TAG_FLAG(sentry_service_send_timeout_seconds, experimental);

DEFINE_int32(sentry_service_recv_timeout_seconds, 60,
             "Configures the socket receive timeout, in seconds, for Thrift "
             "connections to the Sentry server.");
TAG_FLAG(sentry_service_recv_timeout_seconds, advanced);
TAG_FLAG(sentry_service_recv_timeout_seconds, experimental);

DEFINE_int32(sentry_service_conn_timeout_seconds, 60,
             "Configures the socket connect timeout, in seconds, for Thrift "
             "connections to the Sentry server.");
TAG_FLAG(sentry_service_conn_timeout_seconds, advanced);
TAG_FLAG(sentry_service_conn_timeout_seconds, experimental);

DEFINE_int32(sentry_service_max_message_size_bytes, 100 * 1024 * 1024,
             "Maximum size of Sentry objects that can be received by the "
             "Sentry client in bytes. Must match the value of the "
             "sentry.policy.client.thrift.max.message.size option in the "
             "Sentry server configuration.");
TAG_FLAG(sentry_service_max_message_size_bytes, advanced);
TAG_FLAG(sentry_service_max_message_size_bytes, experimental);

using strings::Substitute;

namespace kudu {

using sentry::SentryAction;
using sentry::SentryClient;
using sentry::SentryAuthorizableScope;

namespace master {

// Validates the sentry_service_rpc_addresses gflag.
static bool ValidateAddresses(const char* flag_name, const string& addresses) {
  vector<HostPort> host_ports;
  Status s = HostPort::ParseStringsWithScheme(addresses,
                                              SentryClient::kDefaultSentryPort,
                                              &host_ports);
  if (!s.ok()) {
    LOG(ERROR) << "invalid flag " << flag_name << ": " << s.ToString();
  }
  return s.ok();
}
DEFINE_validator(sentry_service_rpc_addresses, &ValidateAddresses);

SentryAuthzProvider::~SentryAuthzProvider() {
  Stop();
}

Status SentryAuthzProvider::Start() {
  vector<HostPort> addresses;
  RETURN_NOT_OK(HostPort::ParseStringsWithScheme(FLAGS_sentry_service_rpc_addresses,
                                                 SentryClient::kDefaultSentryPort,
                                                 &addresses));

  thrift::ClientOptions options;
  options.enable_kerberos = boost::iequals(FLAGS_sentry_service_security_mode, "kerberos");
  options.service_principal = FLAGS_sentry_service_kerberos_principal;
  options.send_timeout = MonoDelta::FromSeconds(FLAGS_sentry_service_send_timeout_seconds);
  options.recv_timeout = MonoDelta::FromSeconds(FLAGS_sentry_service_recv_timeout_seconds);
  options.conn_timeout = MonoDelta::FromSeconds(FLAGS_sentry_service_conn_timeout_seconds);
  options.max_buf_size = FLAGS_sentry_service_max_message_size_bytes;
  options.retry_count = FLAGS_sentry_service_retry_count;
  return ha_client_.Start(std::move(addresses), std::move(options));
}

void SentryAuthzProvider::Stop() {
  ha_client_.Stop();
}

bool SentryAuthzProvider::IsEnabled() {
  return !FLAGS_sentry_service_rpc_addresses.empty();
}

namespace {

// Returns an authorizable based on the table identifier (in the format
// <database-name>.<table-name>) and the given scope.
Status GetAuthorizable(const string& table_ident,
                       SentryAuthorizableScope::Scope scope,
                       TSentryAuthorizable* authorizable) {
  Slice database;
  Slice table;
  // Authorizable scope for table authorizable type must be equal or higher than
  // 'TABLE'.
  DCHECK_NE(scope, SentryAuthorizableScope::Scope::COLUMN);
  switch (scope) {
    case SentryAuthorizableScope::Scope::TABLE:
      RETURN_NOT_OK(ParseHiveTableIdentifier(table_ident, &database, &table));
      DCHECK(!table.empty());
      authorizable->__set_table(table.ToString());
      FALLTHROUGH_INTENDED;
    case SentryAuthorizableScope::Scope::DATABASE:
      if (database.empty() && table.empty()) {
        RETURN_NOT_OK(ParseHiveTableIdentifier(table_ident, &database, &table));
      }
      DCHECK(!database.empty());
      authorizable->__set_db(database.ToString());
      FALLTHROUGH_INTENDED;
    case SentryAuthorizableScope::Scope::SERVER:
      authorizable->__set_server(FLAGS_server_name);
      break;
    default:
      LOG(FATAL) << "unsupported SentryAuthorizableScope: "
                 << sentry::ScopeToString(scope);
      break;
  }

  return Status::OK();
}

} // anonymous namespace

Status SentryAuthzProvider::AuthorizeCreateTable(const string& table_name,
                                                 const string& user,
                                                 const string& owner) {
  // If the table is being created with a different owner than the user,
  // then the creating user must have 'ALL ON DATABASE' with grant. See
  // design doc in [SENTRY-2151](https://issues.apache.org/jira/browse/SENTRY-2151).
  //
  // Otherwise, table creation requires 'CREATE ON DATABASE' privilege.
  SentryAction::Action action;
  bool grant_option;
  if (user == owner) {
    action = SentryAction::Action::CREATE;
    grant_option = false;
  } else {
    action = SentryAction::Action::ALL;
    grant_option = true;
  }
  return Authorize(SentryAuthorizableScope::Scope::DATABASE, action,
                   table_name, user, grant_option);
}

Status SentryAuthzProvider::AuthorizeDropTable(const string& table_name,
                                               const string& user) {
  // Table deletion requires 'DROP ON TABLE' privilege.
  return Authorize(SentryAuthorizableScope::Scope::TABLE,
                   SentryAction::Action::DROP,
                   table_name, user);
}

Status SentryAuthzProvider::AuthorizeAlterTable(const string& old_table,
                                                const string& new_table,
                                                const string& user) {
  // For table alteration (without table rename) requires 'ALTER ON TABLE'
  // privilege;
  // For table alteration (with table rename) requires
  //  1. 'ALL ON TABLE <old-table>',
  //  2. 'CREATE ON DATABASE <new-database>'.
  // See [SENTRY-2264](https://issues.apache.org/jira/browse/SENTRY-2264).
  // TODO(hao): add inline hierarchy validation to avoid multiple RPCs.
  if (old_table == new_table) {
    return Authorize(SentryAuthorizableScope::Scope::TABLE,
                     SentryAction::Action::ALTER,
                     old_table, user);
  }
  RETURN_NOT_OK(Authorize(SentryAuthorizableScope::Scope::TABLE,
                          SentryAction::Action::ALL,
                          old_table, user));
  return Authorize(SentryAuthorizableScope::Scope::DATABASE,
                   SentryAction::Action::CREATE,
                   new_table, user);
}

Status SentryAuthzProvider::AuthorizeGetTableMetadata(const std::string& table_name,
                                                      const std::string& user) {
  // Retrieving table metadata requires 'METADATA ON TABLE' privilege.
  return Authorize(SentryAuthorizableScope::Scope::TABLE,
                   SentryAction::Action::METADATA,
                   table_name, user);
}

Status SentryAuthzProvider::Authorize(SentryAuthorizableScope::Scope scope,
                                      SentryAction::Action action,
                                      const string& table_ident,
                                      const string& user,
                                      bool require_grant_option) {

  TSentryAuthorizable authorizable;
  RETURN_NOT_OK(GetAuthorizable(table_ident, scope, &authorizable));

  // In general, a privilege implies another when:
  // 1. the authorizable from the former implies the authorizable from the latter
  //    (authorizable with a higher scope on the hierarchy can imply authorizables
  //    with a lower scope on the hierarchy, but not vice versa), and
  // 2. the action from the former implies the action from the latter, and
  // 3. grant option from the former implies the grant option from the latter.
  //
  // See org.apache.sentry.policy.common.CommonPrivilege. Note that policy validation
  // in CommonPrivilege also allows wildcard authorizable matching. For example,
  // authorizable 'server=server1->db=*' can imply authorizable 'server=server1'.
  // However, wildcard authorizable granting is neither practical nor useful (semantics
  // of granting such privilege are not supported in Apache Hive, Impala and Hue. And
  // 'server=server1->db=*' has exactly the same meaning as 'server=server1'). Therefore,
  // wildcard authorizable matching is dropped in this implementation.
  //
  // Moreover, because ListPrivilegesByUser lists all Sentry privileges granted to the
  // user that match the authorizable of each scope in the input authorizable hierarchy,
  // privileges with lower scope will also be returned in the response. This contradicts
  // rule (1) mentioned above. Therefore, we need to validate privilege scope, in addition
  // to action and grant option. Otherwise, privilege escalation can happen.

  TListSentryPrivilegesRequest request;
  request.__set_requestorUserName(FLAGS_kudu_service_name);
  request.__set_principalName(user);
  request.__set_authorizableHierarchy(authorizable);
  TListSentryPrivilegesResponse response;

  RETURN_NOT_OK(ha_client_.Execute(
      [&] (SentryClient* client) {
        return client->ListPrivilegesByUser(request, &response);
      }));

  SentryAction required_action(action);
  SentryAuthorizableScope required_scope(scope);
  for (const auto& privilege : response.privileges) {
    // A grant option cannot imply the other if the latter is set
    // but the former is not.
    if (require_grant_option && privilege.grantOption != TSentryGrantOption::ENABLED) {
      continue;
    }

    SentryAction granted_action;
    Status s = SentryAction::FromString(privilege.action, &granted_action);
    if (!s.ok()) {
      LOG(WARNING) << s.ToString();
      continue;
    }

    SentryAuthorizableScope granted_scope;
    s = SentryAuthorizableScope::FromString(privilege.privilegeScope, &granted_scope);
    if (!s.ok()) {
      LOG(WARNING) << s.ToString();
      continue;
    }

    // Both privilege scope and action need to imply the other.
    if (granted_action.Implies(required_action) &&
        granted_scope.Implies(required_scope)) {
      return Status::OK();
    }
  }

  // Logs a warning if the action is not authorized for debugging purpose, and
  // only returns generic error back to the users to avoid side channel leak,
  // e.g. 'whether table A exists'.
  LOG(WARNING) << Substitute("Action <$0> on table <$1> with authorizable scope "
                             "<$2> is not permitted for user <$3>",
                             sentry::ActionToString(action),
                             table_ident,
                             sentry::ScopeToString(scope),
                             user);
  return Status::NotAuthorized("unauthorized action");
}

} // namespace master
} // namespace kudu
