//////////////////////////////////////////////////////////////////////////
//
// pgAdmin III - PostgreSQL Tools
// 
// Copyright (C) 2002 - 2010, The pgAdmin Development Team
// This software is released under the PostgreSQL Licence
//
// frmBackupServer.cpp - Backup server dialogue
//
//////////////////////////////////////////////////////////////////////////

// wxWindows headers
#include <wx/wx.h>
#include <wx/settings.h>


// App headers
#include "pgAdmin3.h"
#include "frm/frmMain.h"
#include "frm/frmBackupServer.h"
#include "utils/sysLogger.h"
#include "schema/pgSchema.h"
#include "schema/pgTable.h"

// Icons
#include "images/backup.xpm"


#define nbNotebook              CTRL_NOTEBOOK("nbNotebook")
#define txtFilename             CTRL_TEXT("txtFilename")
#define btnFilename             CTRL_BUTTON("btnFilename")
#define cbRolename              CTRL_COMBOBOX("cbRolename")
#define chkVerbose              CTRL_CHECKBOX("chkVerbose")
#define chkForceQuoteForIdent   CTRL_CHECKBOX("chkForceQuoteForIdent")


BEGIN_EVENT_TABLE(frmBackupServer, ExternProcessDialog)
    EVT_TEXT(XRCID("txtFilename"),          frmBackupServer::OnChange)
    EVT_BUTTON(XRCID("btnFilename"),        frmBackupServer::OnSelectFilename)
    EVT_BUTTON(wxID_OK,                     frmBackupServer::OnOK)
    EVT_CLOSE(                              ExternProcessDialog::OnClose)
END_EVENT_TABLE()



frmBackupServer::frmBackupServer(frmMain *form, pgObject *obj) : ExternProcessDialog(form)
{
    object=obj;

    wxWindowBase::SetFont(settings->GetSystemFont());
    LoadResource(form, wxT("frmBackupServer"));
    RestorePosition();

    SetTitle(object->GetTranslatedMessage(BACKUPSERVERTITLE));

    pgServer *server = (pgServer *)object;
    if (server->GetConnection()->EdbMinimumVersion(8,0))
        backupExecutable=edbBackupAllExecutable;
    else if (server->GetConnection()->GetIsGreenplum())
        backupExecutable=gpBackupAllExecutable;
    else
        backupExecutable=pgBackupAllExecutable;

    wxString val;
    settings->Read(wxT("frmBackupServer/LastFile"), &val, wxEmptyString);
    txtFilename->SetValue(val);

    bool roles_supported = pgAppMinimumVersion(backupExecutable, 8, 4) && server->GetConnection()->BackendMinimumVersion(8, 1);
    cbRolename->Enable(roles_supported);

    if (roles_supported)
    {
        // Collect the available rolenames
        pgSetIterator set(server->GetConnection(),
            wxT("SELECT DISTINCT rolname\n")
            wxT("FROM pg_roles db\n")
            wxT("ORDER BY rolname"));

        cbRolename->Append(wxEmptyString);

        while(set.RowsLeft())
            cbRolename->Append(set.GetVal(wxT("rolname")));

        cbRolename->SetValue(server->GetRolename());
    }

    if (!server->GetPasswordIsStored())
       environment.Add(wxT("PGPASSWORD=") + server->GetPassword());

	// Pass the SSL mode via the environment
	environment.Add(wxT("PGSSLMODE=") + server->GetConnection()->GetSslModeName());

	// Icon
    SetIcon(wxIcon(backup_xpm));

    txtMessages = CTRL_TEXT("txtMessages");
    txtMessages->SetMaxLength(0L);
    btnOK->Disable();

    if (!pgAppMinimumVersion(backupExecutable, 9,1))
    {
        chkForceQuoteForIdent->Disable();
    }

    wxCommandEvent ev;
    OnChange(ev);
}


frmBackupServer::~frmBackupServer()
{
    SavePosition();
}


wxString frmBackupServer::GetHelpPage() const
{
    wxString page;
    page = wxT("pg/app-pg-dumpall");
    return page;
}


void frmBackupServer::OnSelectFilename(wxCommandEvent &ev)
{
    wxString title, prompt, FilenameOnly;

    title  = _("Select output file");
#ifdef __WXMSW__
    prompt = _("Query files (*.sql)|*.sql|All files (*.*)|*.*");
#else
    prompt = _("Query files (*.sql)|*.sql|All files (*)|*");
#endif
    
    wxFileName::SplitPath(txtFilename->GetValue(), NULL, NULL, &FilenameOnly, NULL);
    wxFileDialog file(this, title, ::wxPathOnly(txtFilename->GetValue()), FilenameOnly, prompt, wxFD_SAVE);
    
    if (file.ShowModal() == wxID_OK)
    {
        txtFilename->SetValue(file.GetPath());
        OnChange(ev);
    }
}


void frmBackupServer::OnChange(wxCommandEvent &ev)
{
    if (!process && !done)
        btnOK->Enable(!txtFilename->GetValue().IsEmpty());
}

wxString frmBackupServer::GetCmd(int step)
{
    wxString cmd = getCmdPart1();

    return cmd + getCmdPart2();
}


wxString frmBackupServer::GetDisplayCmd(int step)
{
    wxString cmd = getCmdPart1();

    return cmd + getCmdPart2();
}


wxString frmBackupServer::getCmdPart1()
{
    pgServer *server = (pgServer *)object;

    wxString cmd = backupExecutable;

    if (!server->GetName().IsEmpty())
        cmd += wxT(" --host ") + server->GetName();

    cmd +=  wxT(" --port ") + NumToStr((long)server->GetPort())
         +  wxT(" --username ") + commandLineCleanOption(qtIdent(server->GetUsername()));

    if (!cbRolename->GetValue().IsEmpty())
        cmd += wxT(" --role ") + commandLineCleanOption(qtIdent(cbRolename->GetValue()));

    return cmd;
}


wxString frmBackupServer::getCmdPart2()
{
    wxString cmd;

    if (settings->GetIgnoreVersion())
        cmd.Append(wxT(" --ignore-version"));
    if (chkVerbose->GetValue())
        cmd.Append(wxT(" --verbose"));
    if (chkForceQuoteForIdent->GetValue())
        cmd.Append(wxT(" --quote-all-identifiers"));

    cmd.Append(wxT(" --file \"") + txtFilename->GetValue() + wxT("\""));

    return cmd;
}


void frmBackupServer::Go()
{
    txtFilename->SetFocus();
    Show(true);
}

void frmBackupServer::OnOK(wxCommandEvent &ev)
{
    if (!done)
    {
        if (processedFile == txtFilename->GetValue())
        {
            if (wxMessageBox(_("Are you sure you wish to run a backup to this file again?"), _("Repeat backup?"), wxICON_QUESTION | wxYES_NO) == wxNO)
                return;
        }
        else if (wxFile::Exists(txtFilename->GetValue()))
        {
            wxString msg;
            msg.Printf(_("The file: \n\n%s\n\nalready exists. Do you want to overwrite it?"), txtFilename->GetValue().c_str());
            if (wxMessageBox(msg, _("Overwrite file?"), wxICON_WARNING | wxYES_NO) == wxNO)
                return;
        }

        processedFile = txtFilename->GetValue();
    }

    settings->Write(wxT("frmBackupServer/LastFile"), txtFilename->GetValue());
    ExternProcessDialog::OnOK(ev);
}

backupServerFactory::backupServerFactory(menuFactoryList *list, wxMenu *mnu, ctlMenuToolbar *toolbar) : contextActionFactory(list)
{
    mnu->Append(id, _("&Backup server..."), _("Creates a backup of the entire server"));
}


wxWindow *backupServerFactory::StartDialog(frmMain *form, pgObject *obj)
{
    frmBackupServer *frm=new frmBackupServer(form, obj);
    frm->Go();
    return 0;
}


bool backupServerFactory::CheckEnable(pgObject *obj)
{
    if (!obj)
        return false;

    if (!((pgServer *)obj)->GetConnected() || obj->GetMetaType() != PGM_SERVER)
        return false;

    if (obj->GetConnection()->EdbMinimumVersion(8, 0))
        return !edbBackupExecutable.IsEmpty() && pgAppMinimumVersion(edbBackupExecutable, 8, 3);
    else if (obj->GetConnection()->GetIsGreenplum())
        return !gpBackupExecutable.IsEmpty() && pgAppMinimumVersion(gpBackupExecutable, 8, 3);
    else
        return !pgBackupExecutable.IsEmpty() && pgAppMinimumVersion(pgBackupExecutable, 8, 3);
}

