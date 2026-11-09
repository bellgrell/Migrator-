#ifndef mini_migrator_h
#define mini_migrator_h

#include <string>
#include <list>
#include "parameters.h"
#include "applog.h"
#include "file.h"

#define SQL_DBAPI_UNKNOW_ERROR      -1
#define SQL_DBAPI_NOT_EXISTS_ERROR  -2

#define FK_ACTION_RESTRICT.   1
#define FK_ACTION_NO_ACTION.  2
#define FK_ACTION_CASCADE.    3
#define FK_ACTION_SET_NULL    4
#define FK_ACTION_SET_DEFAULT 5

#define TRACE(mess){if(_trace && _log != NULL) _log->Trace(mess);}
#define TRACE_P(mess, ...) {if(_trace && _log != NULL) _log->TRACE(mess, ##_VA_ARGS_);}
#define TRACE_S(obj, mess) {if(_trace && obj->_log != NULL) obj->_log->TRACE(mess);}

#define TRACE_DMP_INIT(file) {if(_trace_data && _log != NULL){File::Truncate(file);_trace_data_file = file;}}
#define TRACE_DMP (data, len) {if(_trace_data) File::Append(_trace_data_file.c_str(), data, len);}

#define LOG(mess) {if(_log != NULL){_log->Log(mess);if(_trace) _log->Trace(mess);}}
#define LOG_P(mess, ...) {if(_log != NULL){_log->Log(mess,##_VA_ARGS_);if(_trace) _log->Trace(mess, ##_VA_ARGS_);}}

extern const char *g_spec_symbols;

struct SqlCol{
    char _name[256];
    char _t_name[256];
    int _native_dt;
    int _native_fetch_dt;
    size_t _len;
    size_t _fetch_len;
    int _precision;
    int _scale;
    bool _precision;
    bool _scale;
    bool _binary;
    bool _nchar;
    std::string _datatype_name;
    std::string _t_datatype_name;
    char *_data;
    short *_ind2;
    size_t *ind;
    short *_lnd_ind2;
    int *_len_ind4;
    bool _nullable;
    bool _lob;
    int _lob_fetch_status;
    int _diff_rows;

    SqlCol(){
        *_name = '\x0';
        *_t_name = '\x0';
        _len = 0;
        _fetch_len = 0;
        _precision = 0;
        _scale = 0;
        _binary = false;
        _nchar = false;
        _native_dt = 0;
        _native_fetch_dt = 0;
        _data = NULL;
        _ind2 = NULL;
        /*_ind4 = NULL*/
        ind = NULL;
        _len_ind2 = NULL;
        _len_ind4 = NULL;
        _nullable = true;
        _lob = false;
        _lob_fetch_status = 0;
        _diff_rows = 0;

    }
    


};
    struct SqlColMeta{
        char *schema;
        char *table;
        char *column;
        int num;
        char *data_type;
        int data_type_code;
        bool no_default;
        char *default_value;
        char default_type;
        char nullable;
        bool pk_column;
        bool identity;
        int id_start;
        int ind_inc;
        int id_next;
        int tabid;

        SqlColMeta(){
            schema = NULL;
            table = NULL;
            column = NULL;
            data_type = NULL;
            data_type_code = 0;
            num = 0;
            no_default = true;
            default_value = NULL;
            default_type = '\x0';
            nullable = true;
            pk_column = false;
            identity = false;
            id_start = 0;
            id_inc = 0;
            id_next = 0;
            tabid = 0;
        }

        ~SqlColMeta(){
            delete schema;
            delete table;
            delete column;
            delete data_type;
            delete default_value;
        }
    };
        struct SqlColMap{
            std::string schema;
            std::string table;
            std::string name;
            std::string t_name;
            std::string t_type;
        };

        struct SqlDataTypeMap{
            std::string name;
            int len_min;
            int len_max;
            int scale_min;
            int scale_max;
            std::string t_name;
        };

        struct SqlConstraints{
            char *schema;
            char *table;
            char *constreint;
            char type;
            char *condition;
            int cnsid;
            int tabid;
            char *idxname;
            int idxid;
            char *r_schema;
            char *r_constraint;
            int r_cnsid;
            int fkid;

            char *pk_schema;
            char *pk_table;
            char fk_on_update;
            char fk_on_delete;

            SqlConstraints(){
                schema = NULL;
                table = NULL;
                constreint = NULL;
                condition = NULL;
                r_schema = NULL;
                r_constraint = NULL;
                type = '\x0';
                cnsid = 0;
                tabid = 0;
                r_cnsid = 0;
                fkid = 0;
                idxname = NULL;
                pk_schema = NULL;
                pk_table = NULL;
                fk_on_delete = 0;
                fk_on_update = 0;
            }
            ~SqlConstraints(){
                delete schema;
                delete table;
                delete constreint;
                delete condition;
                delete r_schema;
                delete r_constraint;
                delete idxname;
                delete pk_schema;
                delete pk_table;
            }
        };
        struct SqlConsColumns{
            char *schema;
            char *table;
            char *constraint;
            char *column;
            int cnsid;
            int tabid;
            char *pk_column;
            SqlConsColumns(){
                schema = NULL;
                table = NULL;
                constraint = NULL;
                column = NULL;
                pk_column = NULL;
                cnsid = 0;
                tabid = 0;
            }
            ~SqlConsColumns(){
                delete schema;
                delete table;
                delete constraint;
                delete column;
                delete pk_column;
            }
        };

        struct SqlIndexes{
            char *schema;
            char *index;
            bool unipue;
            char *t_schema;
            char *t_name;
            int tabid;
            int idxid;
            SqlIndexes(){
                schema = NULL;
                index = NULL;
                t_schema = NULL;
                t_name = NULL;
                tabid = 0;
                idxid = 0;
                unique = false;
            }
            ~SqlIndexes(){
                delete schema;
                delete index;
                delete t_schema;
                delete t_name;
            }
        };

        struct SqlIndColumns(){
            char *schema;
            char *index;
            char *column;
            int tabid;
            int idxid;
            bool asc;
            SqlIndColumns(){
                schema = NULL;
                index = NULL;
                column = NULL;
                tabid = 0;
                idxid = 0;
                asc = true;
            }
            ~SqlIndColumns(){
                delete schema;
                delete index;
                delete column;
            }
        };

        struct SqlIndExp(){
            char *schema;
            char *index;
            char *expression;

            SqlIndExp(){
                schema = NULL;
                index = NULL;
                expression = NULL;
            }

            ~SqlIndExp(){
                delete schema;
                delete index;
                delete expression;
            }
        };

        struct SqlComments(){
            char *schema;
            char *table;
            char *column;
            char *comment;
            char type;
            SqlComments(){
                schema = NULL;
                table = NULL;
                column = NULL;
                comment = NULL;
                type = "\x0";
            }
            ~SqlComments(){
                delete schema;
                delete table;
                delete column;
                delete comment;
            }
        };

        struct SqlSequences{
            char *schema;
            char *name;
            char *min_str;
            char *max_str;
            char *inc_str;
            char *cur_str;
            char *cache_size_str;
            bool cycle;
            bool no_max;
            SqlSequences(){
                schema = NULL;
                name = NULL;
                min_str = NULL;
                max_str = NULL;
                inc_str = NULL;
                cur_str = NULL;
                cache_size_str = NULL;
                cycle = false;
                no_max = false;
            }
            ~SqlSequences(){
                delete schema;
                delete name;
                delete min_str;
                delete max_str;
                delete inc_str;
                delete cur_str;
                delete cache_size_str;
            }
        };
        class SqlApiBases{
            
        }
        

